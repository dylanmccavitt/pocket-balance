#include "SourceIdentity.h"
#include <cstdlib>
#include <iostream>

int main()
{
    struct Example { const char* name; int role; };
    const Example examples[] {
        { "OLIVER_DWILLY_kick_acoustic_pop_shortcake", 0 },
        { "Kick 01", 0 }, { "KICK01.WAV", 0 }, { "Bass Drum", 0 },
        { "bass-drum-kick", 0 }, { "Bassdrum", 0 },
        { "808", 1 }, { "808_long", 1 }, { "TR-808", 1 },
        { "Deep SUB", 1 }, { "SubBass", 1 }, { "Bass_04", 1 },
        { "snare.dry", 2 }, { "RIM_SHOT", 2 }, { "Rimshot02", 2 },
        { "CLAP", 3 }, { "handclap", 3 }, { "808 clap", 3 },
        { "808_snare", 2 }, { "808 kick", 0 },
        { "hihat-electro", 4 }, { "Hi-Hat", 4 }, { "closed_hat", 4 },
        { "OPEN HI HAT", 4 }, { "OpenHiHat", 4 }, { "808 hats", 4 },
        { "Shaker 01", 5 }, { "Tom-low", 5 }, { "Percussion", 5 },
        { "COWBELL", 5 }, { "808 tom", 5 },
        { "Ride.wav", 6 }, { "CRASH", 6 }, { "Cymbals", 6 },
        { "808 crash", 6 }, { "Other", 7 },
        { "kick + snare", -1 }, { "808 kick snare", -1 },
        { "clap/snare layer", -1 }, { "bass and kick", -1 },
        { "ride + hat", -1 }, { "", -1 }, { "   ", -1 },
        { "---_...", -1 }, { "Drum 01", -1 }, { "Input 02", -1 },
        { "shortcake", -1 }, { "kickstart", -1 }, { "bassoon", -1 },
        { "crashing", -1 }, { "tomorrow", -1 }, { "rimless", -1 },
        { "submarine", -1 }, { "clapping", -1 }, { "8080", -1 },
        { "my808sound", -1 }, { "khats", -1 }, { "piano", -1 },
        { "\xC3\xA9_kick_\xE9\x9F\xB3", 0 }
    };

    for (const auto& example : examples)
    {
        const auto actual = pocket::inferRole(example.name);
        if (actual != example.role)
        {
            std::cerr << "FAIL: source label '" << example.name << "' inferred "
                      << actual << ", expected " << example.role << '\n';
            return EXIT_FAILURE;
        }
    }

    std::cout << "PASS: source-label role inference, 808 percussion precedence, "
                 "ambiguous labels and word boundaries\n";
    return EXIT_SUCCESS;
}
