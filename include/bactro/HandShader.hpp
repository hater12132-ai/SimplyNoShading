#pragma once

namespace bactro::handshader {

void registerModule();
void onSignaturesReady();
void onFrame(); // rainbow tick / status
void shutdown();

} // namespace bactro::handshader
