#pragma once
#include <QStringList>
namespace UnixUpdate
{
bool canInstall();
bool startupPending();
// Called only after the restarted GUI has rendered its first frame.
bool acknowledgeStartup();
int apply(const QStringList &arguments);
}
