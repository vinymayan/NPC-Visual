#pragma once
#include <Windows.h>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "SKSEMCP/SKSEMenuFramework.hpp"
#include <miniz.h>

namespace NSettings {
    void NPCMenu();
    void Presets();
    void NPCList();
    void Export();
    void MmRegister();
    void Load();
    void Save();
	void Debug();
}
