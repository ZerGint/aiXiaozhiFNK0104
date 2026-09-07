#ifndef RADIO_STORAGE_H
#define RADIO_STORAGE_H

#include <mutex>
#include <string>
#include <vector>

#include "internet_radio_player.h"

class RadioStorage {
public:
    static RadioStorage& GetInstance();

    // Favorites API (/sdcard/radio_favorites.json)
    bool LoadFavorites(std::vector<RadioStationInfo>& favorites, std::string& err_msg);
    bool SaveFavorites(const std::vector<RadioStationInfo>& favorites, std::string& err_msg);
    std::string AddFavorite(const RadioStationInfo& station);
    std::string RemoveFavorite(const std::string& name);
    std::string ListFavorites();
    bool GetFavorites(std::vector<RadioStationInfo>& favorites);

    // Catalog API (/sdcard/radio_catalog.json)
    bool LoadCatalog(std::vector<RadioStationInfo>& stations, std::string& err_msg);
    bool SaveCatalog(const std::vector<RadioStationInfo>& stations, std::string& err_msg);
    bool AddOrUpdateCatalogStation(const RadioStationInfo& station, std::string& err_msg);
    bool AddOrUpdateCatalogStations(const std::vector<RadioStationInfo>& stations, std::string& err_msg);
    bool GetCatalog(std::vector<RadioStationInfo>& stations);
    std::vector<RadioStationInfo> SearchCatalog(const std::string& query = "",
                                                const std::string& countrycode = "",
                                                const std::string& language = "",
                                                const std::string& tag = "",
                                                int limit = 5);

private:
    RadioStorage() = default;
    ~RadioStorage() = default;
    RadioStorage(const RadioStorage&) = delete;
    RadioStorage& operator=(const RadioStorage&) = delete;

    bool LoadJsonFile(const char* path, std::vector<RadioStationInfo>& stations, std::string& err_msg);
    bool SaveJsonFileAtomic(const char* target_path, const char* tmp_path, const std::vector<RadioStationInfo>& stations, std::string& err_msg);

    bool LoadCatalogInternal(std::vector<RadioStationInfo>& stations, std::string& err_msg);
    bool SaveCatalogInternal(const std::vector<RadioStationInfo>& stations, std::string& err_msg);

    std::mutex mutex_;
};

#endif // RADIO_STORAGE_H
