#include "backend/audiosharing/AudioWorker.h"
#include <QByteArray>

int main(int argc, char** argv) {
    if (argc <= 1 || QByteArray(argv[1]) != QByteArrayLiteral("--audio-worker")) return 64;
    return runAudioWorker(argc, argv);
}
