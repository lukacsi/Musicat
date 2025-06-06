#include "musicat/util/spotify_api.h"
#include "musicat/util/base64.h"
#include "musicat/musicat.h"
#include <list>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>

namespace musicat::util::spotify_api {

static std::string
request(const std::string &url, const std::list<std::string> &headers,
        const std::string &post)
{
    curlpp::Easy req;
    std::ostringstream os;
    req.setOpt(curlpp::options::Url(url));
    req.setOpt(curlpp::options::WriteStream(&os));
    if (!headers.empty())
        req.setOpt(curlpp::options::HttpHeader(headers));
    if (!post.empty()) {
        req.setOpt(curlpp::options::PostFields(post));
        req.setOpt(curlpp::options::PostFieldSize(post.size()));
    }
    if (get_debug_state())
        fprintf(stderr, "[spotify_api] %s %s\n",
                post.empty() ? "GET" : "POST", url.c_str());

    try {
        req.perform();
        std::string res = os.str();
        if (get_debug_state())
            fprintf(stderr, "[spotify_api] response: %s\n", res.c_str());
        return res;
    } catch (std::exception &e) {
        fprintf(stderr, "[spotify_api ERROR] %s: %s\n", url.c_str(), e.what());
        return "";
    }
}

std::string
authenticate(const std::string &client_id, const std::string &client_secret)
{
    std::string creds = client_id + ":" + client_secret;
    std::string auth =
        "Authorization: Basic " + util::base64::encode_standard(creds);
    std::list<std::string> headers = {
        "Content-Type: application/x-www-form-urlencoded", auth };
    std::string body = "grant_type=client_credentials";
    auto res = request("https://accounts.spotify.com/api/token", headers, body);
    auto j = nlohmann::json::parse(res, nullptr, false);
    if (!j.is_object())
        return "";
    return j.value("access_token", "");
}

static bool
parse_url(const std::string &url, std::string &type, std::string &id)
{
    std::regex r(R"((?:spotify[/:]|open\.spotify\.com/)(track|playlist)[/:]([^?\n]+))");
    std::smatch m;
    if (std::regex_search(url, m, r)) {
        type = m[1].str();
        id = m[2].str();
        return true;
    }
    return false;
}

std::vector<Track>
fetch_tracks(const std::string &url_or_id, const std::string &access_token)
{
    std::string type;
    std::string id;
    if (!parse_url(url_or_id, type, id))
        return {};

    std::list<std::string> headers = { "Authorization: Bearer " + access_token };
    std::vector<Track> out;

    if (type == "track") {
        std::string url =
            "https://api.spotify.com/v1/tracks/" + id +
            "?fields=name,artists(name)";
        auto res = request(url, headers, "");
        auto j = nlohmann::json::parse(res, nullptr, false);
        if (j.is_object()) {
            Track t;
            t.title = j.value("name", "");
            if (j.contains("artists") && j["artists"].is_array() &&
                j["artists"].size() > 0)
                t.artist = j["artists"][0].value("name", "");
            if (!t.title.empty())
                out.push_back(t);
        }
    } else if (type == "playlist") {
        std::string url =
            "https://api.spotify.com/v1/playlists/" + id +
            "/tracks?fields=items(track(name,artists(name)))&limit=100";
        auto res = request(url, headers, "");
        auto j = nlohmann::json::parse(res, nullptr, false);
        if (j.contains("items") && j["items"].is_array()) {
            for (auto &it : j["items"]) {
                if (!it.contains("track"))
                    continue;
                auto &tr = it["track"];
                Track t;
                t.title = tr.value("name", "");
                if (tr.contains("artists") && tr["artists"].is_array() &&
                    tr["artists"].size() > 0)
                    t.artist = tr["artists"][0].value("name", "");
                if (!t.title.empty())
                    out.push_back(t);
            }
        }
    }
    return out;
}

std::vector<Track>
fetch_tracks(const std::string &url_or_id, const std::string &client_id,
             const std::string &client_secret)
{
    auto token = authenticate(client_id, client_secret);
    if (token.empty())
        return {};
    return fetch_tracks(url_or_id, token);
}

} // namespace musicat::util::spotify_api

