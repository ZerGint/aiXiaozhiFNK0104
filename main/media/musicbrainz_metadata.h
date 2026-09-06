#ifndef MUSICBRAINZ_METADATA_H
#define MUSICBRAINZ_METADATA_H

#include <string>
#include <unordered_map>
#include <mutex>

struct MusicMetadata {
    std::string artist;
    std::string title;
    std::string genre;
};

class MusicBrainzMetadata {
public:
    static MusicBrainzMetadata& GetInstance();
    MusicMetadata Lookup(const std::string& path);

private:
    MusicBrainzMetadata() = default;
    std::string UrlEncode(const std::string& value) const;
    MusicMetadata LookupRemote(const MusicMetadata& guess);

    std::unordered_map<std::string, MusicMetadata> cache_;
    std::mutex mutex_;
};

#endif
