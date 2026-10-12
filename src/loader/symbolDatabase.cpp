#include "loader/symbolDatabase.h"

#include "common/file.h"

#include <fmt/format.h>
#include <magic_enum.hpp>

namespace Loader {

constexpr char LIB_PREFIX[] = "libSce";

static std::string UpdateName(const std::string& str) {
	return str.starts_with(LIB_PREFIX) ? Common::RemoveFirst(str, 6) : str;
}

std::string SymbolDatabase::GenerateName(const SymbolResolve& s) {
	auto library = UpdateName(s.library);
	auto module  = UpdateName(s.module);
	return fmt::format("{}[{}_v{}][{}_v{}.{}][{}]", s.name.c_str(), library.c_str(),
	                   s.library_version, module.c_str(), s.module_version_major,
	                   s.module_version_minor, magic_enum::enum_name(s.type));
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr) {
	Add(s, vaddr, {});
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const std::string& dbg_name) {
	auto name = GenerateName(s);
	const auto existing = m_map.find(name);
	if (existing != m_map.end()) {
		// Resolving the same NID again updates the original record rather than
		// appending unreachable duplicates and invalidating cached pointers.
		auto& record = m_symbols[existing->second];
		record.vaddr = vaddr;
		record.dbg_name = dbg_name;
		return;
	}

	SymbolRecord record {};
	record.name = std::move(name);
	record.vaddr = vaddr;
	record.dbg_name = dbg_name;
	const auto index = m_symbols.size();
	m_symbols.push_back(std::move(record));
	m_map.emplace(m_symbols[index].name, index);
}

void SymbolDatabase::DbgDump(const std::string& folder, const std::string& file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	for (const auto& sym: m_symbols) {
		f.Printf("%" PRIx64 " %s\n", sym.vaddr, sym.name.c_str());
	}

	f.Close();
}

const SymbolRecord* SymbolDatabase::Find(const SymbolResolve& s) const {
	auto it = m_map.find(GenerateName(s));
	if (it == m_map.end()) {
		return nullptr;
	}
	auto index = it->second;
	if (index >= m_symbols.size()) {
		return nullptr;
	}
	return &m_symbols[index];
}

const SymbolRecord* SymbolDatabase::FindByNid(const std::string& nid, SymbolType type,
                                                uint64_t vaddr) const {
	auto prefix = nid + "[";
	auto suffix = fmt::format("[{}]", magic_enum::enum_name(type));

	for (const auto& symbol: m_symbols) {
		if ((vaddr == 0 || symbol.vaddr == vaddr) && symbol.name.starts_with(prefix) &&
		    symbol.name.ends_with(suffix)) {
			return &symbol;
		}
	}

	return nullptr;
}

const SymbolRecord* SymbolDatabase::FindByName(const std::string& name, SymbolType type) const {
	auto prefix = name + "[";
	auto suffix = fmt::format("[{}]", magic_enum::enum_name(type));

	for (const auto& symbol: m_symbols) {
		if (symbol.name.starts_with(prefix) && symbol.name.ends_with(suffix)) {
			return &symbol;
		}
	}

	return nullptr;
}

} // namespace Loader
