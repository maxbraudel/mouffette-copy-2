#pragma once

#include <QStringList>

class QApplication;

int runMultiInstanceProtocolWorker(QApplication& app, const QStringList& arguments);
