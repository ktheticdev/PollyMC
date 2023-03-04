// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
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
 */

#pragma once

#include "BaseInstance.h"
#include "MMCZip.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "tasks/Task.h"

class ModrinthPackExportTask : public Task {
   public:
    ModrinthPackExportTask(const QString& name,
                           const QString& version,
                           const QString& summary,
                           InstancePtr instance,
                           const QString& output,
                           MMCZip::FilterFunction filter);

   protected:
    void executeTask() override;
    bool abort() override;

   private:
    struct ResolvedFile {
        QString sha1, sha512, url;
        int size;
    };

    static const QStringList PREFIXES;

    // inputs
    const QString name, version, summary;
    const InstancePtr instance;
    const QString output;
    const MMCZip::FilterFunction filter;

    ModrinthAPI api;
    QFileInfoList files;
    QMap<QString, QString> pendingHashes;
    QMap<QString, ResolvedFile> resolvedFiles;
    Task::Ptr task;

    void collectFiles();
    void parseApiResponse(QByteArray* response);
    void buildZip();

    QByteArray generateIndex();
};