#ifndef MUSICAT_UTIL_SPOTIFY_API_H
#define MUSICAT_UTIL_SPOTIFY_API_H

#include <string>
#include <vector>

namespace musicat::util::spotify_api {

struct Track {
    std::string artist;
    std::string title;
};

std::string authenticate(const std::string &client_id,
                         const std::string &client_secret);

std::vector<Track> fetch_tracks(const std::string &url_or_id,
                                const std::string &access_token);
std::vector<Track> fetch_tracks(const std::string &url_or_id,
                                const std::string &client_id,
                                const std::string &client_secret);

} // namespace musicat::util::spotify_api

#endif // MUSICAT_UTIL_SPOTIFY_API_H
