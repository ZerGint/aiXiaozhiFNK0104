#pragma once
#include "radio_binary_codec.h"
#include <cstdio>
#include <cstdint>
#include <string>
namespace RadioBinaryIO {
class CatalogReader { public: explicit CatalogReader(const char* path):path_(path){} ~CatalogReader(); bool Open(uint32_t&); bool ReadNext(RadioStationInfo&); bool Finish(); private: const char* path_; FILE* file_{nullptr}; uint32_t remaining_{0}; bool failed_{false}; friend bool CloseReader(CatalogReader&); };
class FavoritesReader { public: explicit FavoritesReader(const char* path):path_(path){} ~FavoritesReader(); bool Open(uint32_t&); bool ReadNext(std::string&); bool Finish(); private: const char* path_; FILE* file_{nullptr}; uint32_t remaining_{0}; bool failed_{false}; friend bool CloseReader(FavoritesReader&); };
class CatalogWriter { public: CatalogWriter(const char* path,uint32_t count):path_(path),count_(count){} ~CatalogWriter(); bool Begin(); bool Write(const RadioStationInfo&); bool Finish(); private: const char* path_; FILE* file_{nullptr}; uint32_t count_,written_{0}; bool failed_{false}; };
class FavoritesWriter { public: FavoritesWriter(const char* path,uint32_t count):path_(path),count_(count){} ~FavoritesWriter(); bool Begin(); bool Write(const std::string&); bool Finish(); private: const char* path_; FILE* file_{nullptr}; uint32_t count_,written_{0}; bool failed_{false}; };
bool ReplaceTarget(const char* tmp_path,const char* target_path);
}
