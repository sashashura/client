#include "spacesbrowser.h"
#include "ui_spacesbrowser.h"


#include "spacesmodel.h"

#include "graphapi/drives.h"

#include <QTimer>


SpacesBrowser::SpacesBrowser(OCC::AccountPtr acc, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SpacesBrowser)
    , _acc(acc)
{
    ui->setupUi(this);
    _model = new SpacesModel(this);
    ui->tableView->setModel(_model);


    QTimer::singleShot(5000, this, [this] {
        auto drive = new OCC::GraphApi::Drives(_acc);
        connect(drive, &OCC::GraphApi::Drives::finishedSignal, [drive, this] {
            _model->setData(drive->drives());
            show();
        });
        drive->start();
    });
}

SpacesBrowser::~SpacesBrowser()
{
    delete ui;
}
