#include <iostream>
using namespace std;

#include <QCoreApplication>
#include <QTimer>

#include "example-qt.h"

void getCallback(siderAsyncContext *, void * r, void * privdata) {

    siderReply * reply = static_cast<siderReply *>(r);
    ExampleQt * ex = static_cast<ExampleQt *>(privdata);
    if (reply == nullptr || ex == nullptr) return;

    cout << "key: " << reply->str << endl;

    ex->finish();
}

void ExampleQt::run() {

    m_ctx = siderAsyncConnect("localhost", 6379);

    if (m_ctx->err) {
        cerr << "Error: " << m_ctx->errstr << endl;
        siderAsyncFree(m_ctx);
        emit finished();
    }

    m_adapter.setContext(m_ctx);

    siderAsyncCommand(m_ctx, NULL, NULL, "SET key %s", m_value);
    siderAsyncCommand(m_ctx, getCallback, this, "GET key");
}

int main (int argc, char **argv) {

    QCoreApplication app(argc, argv);

    ExampleQt example(argv[argc-1]);

    QObject::connect(&example, SIGNAL(finished()), &app, SLOT(quit()));
    QTimer::singleShot(0, &example, SLOT(run()));

    return app.exec();
}
