#pragma once


#include <QWidget>

#include "account.h"

class SpacesModel;

namespace Ui {
class SpacesBrowser;
}

class SpacesBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit SpacesBrowser(OCC::AccountPtr acc, QWidget *parent = nullptr);
    ~SpacesBrowser();

private:
    Ui::SpacesBrowser *ui;

    OCC::AccountPtr _acc;
    SpacesModel *_model;
};
