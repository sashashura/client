#pragma once

#include <QAbstractItemModel>

#include <graphapi/drives.h>

class SpacesModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum class Drives {
        Name,
        Description,
        WebUrl,
        WebDavUrl,

        ColumnCount
    };
    Q_ENUM(Drives)
    explicit SpacesModel(QObject *parent = nullptr);

    // Header:
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setData(const QList<OpenAPI::OAIDrive> &data);

private:
    QList<OpenAPI::OAIDrive> _data;
};
