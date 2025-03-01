/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */
#pragma once

#include "accountfwd.h"

#include <graphapi/drives.h>

#include <QAbstractItemModel>

namespace OCC::Spaces {
class SpacesModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum class Columns {
        Image,
        Name,
        Description,
        WebUrl,
        WebDavUrl,

        ColumnCount
    };
    Q_ENUM(Columns)
    explicit SpacesModel(QObject *parent = nullptr);

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setData(OCC::AccountPtr acc, const QList<OpenAPI::OAIDrive> &data);

private:
    QList<OpenAPI::OAIDrive> _data;

    mutable QHash<QString, QPixmap> _images;
    OCC::AccountPtr _acc;
};
}