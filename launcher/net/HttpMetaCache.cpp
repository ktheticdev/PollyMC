// SPDX-License-Identifier: GPL-3.0-only
/*
 *  PolyMC - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "HttpMetaCache.h"
#include "FileSystem.h"
#include "Json.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <QDebug>

auto MetaEntry::getFullPath() -> QString
{
    // FIXME: make local?
    return FS::PathCombine(basePath, relativePath);
}

HttpMetaCache::HttpMetaCache(QString path) : QObject(), m_index_file(path)
{
    saveBatchingTimer.setSingleShot(true);
    saveBatchingTimer.setTimerType(Qt::VeryCoarseTimer);

    connect(&saveBatchingTimer, SIGNAL(timeout()), SLOT(SaveNow()));
}

HttpMetaCache::~HttpMetaCache()
{
    saveBatchingTimer.stop();
    SaveNow();
}

auto HttpMetaCache::getEntry(QString base, QString resource_path) -> MetaEntryPtr
{
    // no base. no base path. can't store
    if (!m_entries.contains(base)) {
        // TODO: log problem
        return {};
    }

    EntryMap& map = m_entries[base];
    if (map.entry_list.contains(resource_path)) {
        return map.entry_list[resource_path];
    }

    return {};
}

auto HttpMetaCache::resolveEntry(QString base, QString resource_path, QString expected_etag) -> MetaEntryPtr
{
    auto entry = getEntry(base, resource_path);
    // it's not present? generate a default stale entry
    if (!entry) {
        return staleEntry(base, resource_path);
    }

    auto& selected_base = m_entries[base];
    QString real_path = FS::PathCombine(selected_base.base_path, resource_path);
    QFileInfo finfo(real_path);

    // is the file really there? if not -> stale
    if (!finfo.isFile() || !finfo.isReadable()) {
        // if the file doesn't exist, we disown the entry
        selected_base.entry_list.remove(resource_path);
        return staleEntry(base, resource_path);
    }

    if (!expected_etag.isEmpty() && expected_etag != entry->etag) {
        // if the etag doesn't match expected, we disown the entry
        selected_base.entry_list.remove(resource_path);
        return staleEntry(base, resource_path);
    }

    // if the file changed, check md5sum
    qint64 file_last_changed = finfo.lastModified().toUTC().toMSecsSinceEpoch();
    if (file_last_changed != entry->local_changed_timestamp) {
        QFile input(real_path);
        input.open(QIODevice::ReadOnly);
        QString md5sum = QCryptographicHash::hash(input.readAll(), QCryptographicHash::Md5).toHex().constData();
        if (entry->md5sum != md5sum) {
            selected_base.entry_list.remove(resource_path);
            return staleEntry(base, resource_path);
        }

        // md5sums matched... keep entry and save the new state to file
        entry->local_changed_timestamp = file_last_changed;
        SaveEventually();
    }

    // entry passed all the checks we cared about.
    entry->basePath = getBasePath(base);
    return entry;
}

auto HttpMetaCache::updateEntry(MetaEntryPtr stale_entry) -> bool
{
    if (!m_entries.contains(stale_entry->baseId)) {
        qCritical() << "Cannot add entry with unknown base: " << stale_entry->baseId.toLocal8Bit();
        return false;
    }

    if (stale_entry->stale) {
        qCritical() << "Cannot add stale entry: " << stale_entry->getFullPath().toLocal8Bit();
        return false;
    }

    m_entries[stale_entry->baseId].entry_list[stale_entry->relativePath] = stale_entry;
    SaveEventually();

    return true;
}

auto HttpMetaCache::evictEntry(MetaEntryPtr entry) -> bool
{
    if (!entry)
        return false;

    entry->stale = true;
    SaveEventually();
    return true;
}

auto HttpMetaCache::staleEntry(QString base, QString resource_path) -> MetaEntryPtr
{
    auto foo = new MetaEntry();
    foo->baseId = base;
    foo->basePath = getBasePath(base);
    foo->relativePath = resource_path;
    foo->stale = true;

    return MetaEntryPtr(foo);
}

void HttpMetaCache::addBase(QString base, QString base_root)
{
    // TODO: report error
    if (m_entries.contains(base))
        return;

    // TODO: check if the base path is valid
    EntryMap foo;
    foo.base_path = base_root;
    m_entries[base] = foo;
}

auto HttpMetaCache::getBasePath(QString base) -> QString
{
    if (m_entries.contains(base)) {
        return m_entries[base].base_path;
    }

    return {};
}

void HttpMetaCache::Load()
{
    if (m_index_file.isNull())
        return;

    QFile index(m_index_file);
    if (!index.open(QIODevice::ReadOnly))
        return;

    QJsonDocument json = QJsonDocument::fromJson(index.readAll());

    auto root = Json::requireObject(json, "HttpMetaCache root");

    // check file version first
    auto version_val = Json::ensureString(root, "version");
    if (version_val != "1")
        return;

    // read the entry array
    auto array = Json::ensureArray(root, "entries");
    for (auto element : array) {
        auto element_obj = Json::ensureObject(element);
        auto base = Json::ensureString(element_obj, "base");
        if (!m_entries.contains(base))
            continue;

        auto& entrymap = m_entries[base];

        auto foo = new MetaEntry();
        foo->baseId = base;
        foo->relativePath = Json::ensureString(element_obj, "path");
        foo->md5sum = Json::ensureString(element_obj, "md5sum");
        foo->etag = Json::ensureString(element_obj, "etag");
        foo->local_changed_timestamp = Json::ensureDouble(element_obj, "last_changed_timestamp");
        foo->remote_changed_timestamp = Json::ensureString(element_obj, "remote_changed_timestamp");
        // presumed innocent until closer examination
        foo->stale = false;

        entrymap.entry_list[foo->relativePath] = MetaEntryPtr(foo);
    }
}

void HttpMetaCache::SaveEventually()
{
    // reset the save timer
    saveBatchingTimer.stop();
    saveBatchingTimer.start(30000);
}

void HttpMetaCache::SaveNow()
{
    if (m_index_file.isNull())
        return;

    QJsonObject toplevel;
    Json::writeString(toplevel, "version", "1");

    QJsonArray entriesArr;
    for (auto group : m_entries) {
        for (auto entry : group.entry_list) {
            // do not save stale entries. they are dead.
            if (entry->stale) {
                continue;
            }

            QJsonObject entryObj;
            Json::writeString(entryObj, "base", entry->baseId);
            Json::writeString(entryObj, "path", entry->relativePath);
            Json::writeString(entryObj, "md5sum", entry->md5sum);
            Json::writeString(entryObj, "etag", entry->etag);
            entryObj.insert("last_changed_timestamp", QJsonValue(double(entry->local_changed_timestamp)));
            if (!entry->remote_changed_timestamp.isEmpty())
                entryObj.insert("remote_changed_timestamp", QJsonValue(entry->remote_changed_timestamp));
            entriesArr.append(entryObj);
        }
    }
    toplevel.insert("entries", entriesArr);

    try {
        Json::write(toplevel, m_index_file);
    } catch (const Exception& e) {
        qWarning() << e.what();
    }
}
