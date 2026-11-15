// Stand ins the SDK's headless harness does not have yet (the runtime grew past it):
// the audio output's stop and the system language.
#include <string>

#include "app/app.h"
#include "audio/audio.h"

namespace audio {
void StopOutput() {}
}  // namespace audio

namespace app {
std::string SystemLanguage() { return "en"; }
}  // namespace app
