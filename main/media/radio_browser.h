#ifndef RADIO_BROWSER_H
#define RADIO_BROWSER_H

#include <string>
#include <vector>

class RadioBrowser {
public:
    static RadioBrowser& GetInstance();

    void RegisterMcpTools();
    std::string SearchStations(const std::string& query = "",
                               const std::string& countrycode = "",
                               const std::string& language = "",
                               const std::string& tag = "",
                               int limit = 5);
    std::string AddFavorite(const std::string& name, const std::string& country,
                            const std::string& city, const std::string& keywords,
                            const std::string& station_uuid);
    std::string ListFavorites() const;
    std::string PlayFavorite(const std::string& name);
    std::string RemoveFavorite(const std::string& name);

private:
    RadioBrowser() = default;
    std::string PerformRequest(const std::string& path);
    std::string UrlEncode(const std::string& value) const;
    std::string server_url_ = "http://de1.api.radio-browser.info";
    std::vector<std::string> mirrors_ = {
        "http://de1.api.radio-browser.info",
        "http://nl1.api.radio-browser.info",
        "http://at1.api.radio-browser.info",
        "http://fi1.api.radio-browser.info"
    };
    size_t current_mirror_index_ = 0;
};

#endif
