#include "spacesmodel.h"

SpacesModel::SpacesModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

QVariant SpacesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        const auto actionRole = static_cast<Drives>(section);
        switch (role) {
        case Qt::DisplayRole:
            switch (actionRole) {
            case Drives::Name:
                return tr("Name");
            case Drives::Description:
                return tr("Description");
            case Drives::WebUrl:
                return tr("Web URL");
            case Drives::WebDavUrl:
                return tr("Web Dav URL");
            case Drives::ColumnCount:
                Q_UNREACHABLE();
                break;
            }
        }
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

int SpacesModel::rowCount(const QModelIndex &parent) const
{
    Q_ASSERT(checkIndex(parent));
    if (parent.isValid())
        return 0;
    return static_cast<int>(_data.size());
}

int SpacesModel::columnCount(const QModelIndex &parent) const
{
    Q_ASSERT(checkIndex(parent));
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(Drives::ColumnCount);
}

QVariant SpacesModel::data(const QModelIndex &index, int role) const
{
    Q_ASSERT(checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid));

    const auto column = static_cast<Drives>(index.column());
    const auto &item = _data.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        switch (column) {
        case Drives::Name:
            return item.getName();
        case Drives::Description:
            return item.getDescription();
        case Drives::WebUrl:
            return item.getWebUrl();
        case Drives::WebDavUrl:
            return item.getRoot().getWebDavUrl();
        case Drives::ColumnCount:
            Q_UNREACHABLE();
            break;
        }
        break;
    }
    return {};
}

void SpacesModel::setData(const QList<OpenAPI::OAIDrive> &data)
{
    beginResetModel();
    _data = data;
    endResetModel();
}
