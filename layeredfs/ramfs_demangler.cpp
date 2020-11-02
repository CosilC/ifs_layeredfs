/*
	IIDX mounts sounds files annoyingly, for example
	/data/sound/XXX.ifs -> /sdXXX/XXX/
	If avs_fs_mount is called with a known good source but known bad dest, save the mapping
	so it can be demangled later.

	Several games load IFS files into RAM for faster loads.
	Some games (SDVX, mostly) will load these into paths which contain their source, which is fine.
	Some games (Nostalgia with some IFS files) will use a random/different path.

	How do we detect and override this?
	The sequence is as follows:
		avs_fs_lstat to get the size (we ignore this)
		avs_fs_open the ifs file
		avs_fs_read its entire contents into a buffer just alloced
		avs_fs_mount with type ramfs
			options including base=%p, pointing at the buffer fs_read used
			path join mountpoint + root to get the virtual filename
		avs_fs_mount root=ram path, mountpoint = something dumb, type = imagefs

	So,
		avs_fs_open - save mapping from handle -> filename
		avs_fs_read - save mapping from buffer address -> (handle ->) filename
		avs_fs_mount - save mapping from ramfs filename -> (buffer -> handle ->) filename
		avs_fs_mount - save mapping from imagefs filename -> (ramfs -> buffer -> handle ->) filename
		normalise_path - map imagefs filename to real filename before checking mods folder

		As long as the mappings are unique (filename -> x instead of x -> filename)
		we won't leak memory as there is a finite number of filenames to map to.
		At worst, maybe a meg of memory will be lost to saving filename mappings.
*/

#include <algorithm>
#include <unordered_map>
#include <optional>

#include "3rd_party/hat-trie/htrie_map.h"

#include "ramfs_demangler.h"
#include "utils.h"

using namespace std;

typedef struct {
	AVS_FILE handle;
	void* buffer;
	optional<string> ramfs_path;
	optional<string> mounted_path;
} file_cleanup_info_t;

unordered_map<string, file_cleanup_info_t> cleanup_map;
unordered_map<AVS_FILE, string> open_file_map;
unordered_map<void*, string> ram_load_map;
// using tries for fast prefix matches on our mangled names
tsl::htrie_map<char, string> ramfs_map;
tsl::htrie_map<char, string> mangling_map;

void ramfs_demangler_on_fs_open(const std::string& norm_path, AVS_FILE open_result) {
	if (open_result < 0 || !string_ends_with(norm_path.c_str(), ".ifs")) {
		return;
	}

	auto existing_info = cleanup_map.find(norm_path);
	if (existing_info != cleanup_map.end()) {
		file_cleanup_info_t cleanup = existing_info->second;

		open_file_map.erase(cleanup.handle);
		if (cleanup.buffer != NULL) {
			ram_load_map.erase(cleanup.buffer);
		}
		if (cleanup.ramfs_path) {
			ramfs_map.erase(*cleanup.ramfs_path);
		}
		if (cleanup.mounted_path) {
			mangling_map.erase(*cleanup.mounted_path);
		}
		cleanup_map.erase(existing_info);
	}
	file_cleanup_info_t cleanup = {
		open_result,
		NULL,
		nullopt,
		nullopt
	};
	cleanup_map[norm_path] = cleanup;
	open_file_map[open_result] = norm_path;
}

void ramfs_demangler_on_fs_read(AVS_FILE context, void* dest) {
	auto find = open_file_map.find(context);
	if (find != open_file_map.end()) {
		auto path = find->second;
		// even this is too verbose
		//logf_verbose("Mapped %p to %s", dest, path.c_str());
		ram_load_map[dest] = path;

		auto cleanup = cleanup_map.find(path);
		if (cleanup != cleanup_map.end()) {
			cleanup->second.buffer = dest;
		}
	}
}

void ramfs_demangler_on_fs_mount(const char* mountpoint, const char* fsroot, const char* fstype, const char* flags) {
	if (!strcmp(fstype, "ramfs")) {
		void* buffer;

		if (!flags) {
			logf_verbose("ramfs has no flags?");
			return;
		}
		const char* baseptr = strstr(flags, "base=");
		if (!baseptr) {
			logf_verbose("ramfs has no base pointer?");
			return;
		}

		buffer = (void*)strtoull(baseptr + strlen("base="), NULL, 0);
		
		auto find = ram_load_map.find(buffer);
		if (find != ram_load_map.end()) {
			auto orig_path = find->second;
			logf_verbose("ramfs mount mapped to %s", orig_path.c_str());
			string mount_path = (string)mountpoint + "/" + fsroot;
			ramfs_map[mount_path.c_str()] =  orig_path;

			auto cleanup = cleanup_map.find(orig_path);
			if (cleanup != cleanup_map.end()) {
				cleanup->second.ramfs_path = mount_path;
			}
		}
	}
	else if (!strcmp(fstype, "imagefs")) {
		auto find = ramfs_map.longest_prefix(fsroot);
		if (find != ramfs_map.end()) {
			auto orig_path = *find;
			logf_verbose("imagefs mount mapped to %s", orig_path.c_str());
			mangling_map[mountpoint] = orig_path;

			auto cleanup = cleanup_map.find(orig_path);
			if (cleanup != cleanup_map.end()) {
				cleanup->second.mounted_path = mountpoint;
			}
		}
		else if(string_ends_with(fsroot, ".ifs")) {
			//logf_verbose("imagefs mount mapped to %s", fsroot);
			mangling_map[mountpoint] =  (string)fsroot;
		}
	}
}

void ramfs_demangler_demangle_if_possible(std::string& raw_path) {
	auto search = mangling_map.longest_prefix(raw_path);
	if (search != mangling_map.end()) {
		//logf_verbose("can demangle %s to %s", search.key().c_str(), search->c_str());
		string_replace(raw_path, search.key().c_str(), search->c_str());
	}
}