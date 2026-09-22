#pragma once

namespace bactro::motionblur {

void registerModule();
void onSignaturesReady(); // optional; blur uses EGL/GLES only
void shutdown();

} // namespace bactro::motionblur
