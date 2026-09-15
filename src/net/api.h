// The portal's HTTP endpoints (FirmwareSpec.md §15.4): registered once on the
// WebServer; every handler runs on the net task and hands anything that
// touches module state to the app task through Portal::onApp().
#pragma once
class Portal;
namespace Api {
void bind(Portal& p);
}
