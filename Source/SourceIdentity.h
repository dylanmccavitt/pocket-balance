#pragma once

#include <array>
#include <string>
#include <vector>

namespace pocket
{
// Infer a musical role from a user-supplied source label. This does not discover
// host routing or identify audio. Return -1 when there is no clear single role.
// Indices match BalanceEngine.h: kick, bass, snare, clap, hat, perc, cymbal, other.
inline int inferRole(const std::string& name)
{
    std::vector<std::string> tokens;
    std::string token;
    for (const unsigned char character : name)
    {
        const char lower = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : static_cast<char>(character);
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9'))
            token += lower;
        else if (!token.empty())
        {
            tokens.push_back(token);
            token.clear();
        }
    }
    if (!token.empty()) tokens.push_back(token);

    // A numeric sample suffix is safe (kick01); arbitrary substrings are not
    // (kickstart, bassoon, rimless). Non-ASCII bytes act as delimiters.
    const auto matches = [](const std::string& candidate, const char* word)
    {
        const std::string expected(word);
        if (candidate == expected) return true;
        if (candidate.size() <= expected.size()
            || candidate.compare(0, expected.size(), expected) != 0) return false;
        for (auto i = expected.size(); i < candidate.size(); ++i)
            if (candidate[i] < '0' || candidate[i] > '9') return false;
        return true;
    };

    std::array<bool, 8> found {};
    bool has808 = false;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        const auto& current = tokens[index];
        const auto is = [&](const char* word) { return matches(current, word); };

        // "Bass drum" names a kick, rather than a separate bass instrument.
        if (is("bass") && index + 1 < tokens.size()
            && (matches(tokens[index + 1], "drum") || matches(tokens[index + 1], "drums")))
        {
            found[0] = true;
            ++index;
        }
        else if (is("kick") || is("kicks") || is("kickdrum") || is("bassdrum")) found[0] = true;
        else if (is("bass") || is("sub") || is("subbass")) found[1] = true;
        else if (is("snare") || is("snares") || is("rim") || is("rimshot")) found[2] = true;
        else if (is("clap") || is("claps") || is("handclap")) found[3] = true;
        else if (is("hihat") || is("hihats") || is("hat") || is("hats")
                 || is("closedhat") || is("openhat") || is("closedhihat") || is("openhihat")) found[4] = true;
        else if (is("perc") || is("percussion") || is("shaker") || is("shakers")
                 || is("tom") || is("toms") || is("tambourine") || is("cowbell")) found[5] = true;
        else if (is("ride") || is("crash") || is("cymbal") || is("cymbals")) found[6] = true;
        else if (current == "other") found[7] = true;
        else if (current == "808") has808 = true;
    }

    int result = -1;
    for (int role = 0; role < static_cast<int>(found.size()); ++role)
    {
        if (!found[static_cast<std::size_t>(role)]) continue;
        if (result != -1) return -1;
        result = role;
    }

    // 808 often identifies a drum machine/sample collection. A more specific
    // label such as "808 clap" wins; bare "808" conventionally means bass.
    return result != -1 ? result : has808 ? 1 : -1;
}
}
