export module dcc.backend.i8086.image;

import std;
import dcc.ir;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.backend.x86.modasm;
import dcc.backend.object.layout;
import dcc.backend.i8086.link;
export import dcc.backend.object.image;

export namespace dcc::backend::i8086
{
    [[nodiscard]] object::ImageDescription collect_image(ir::IrModule const& module, x86::MModule const& machine, std::vector<x86::EncodeResult> const& encoded,
                                                         target::TargetConfig const& target, x86::ModuleAsmObject const* assembly = nullptr)
    {
        using namespace object;
        using namespace x86;
        ImageDescription image;
        image.assembly_sources.assign(module.module_asms.begin(), module.module_asms.end());
        if (assembly)
            image.assembly = *assembly;
        std::unordered_map<std::string, SymbolId> names;
        auto symbol = [&](std::string_view name) -> ImageSymbol& {
            auto [it, inserted] = names.try_emplace(std::string{name}, image.symbols.size());
            if (inserted)
            {
                ImageSymbol value;
                value.id = it->second;
                value.name = name;
                value.binding = ImageBinding::Global;
                if (name == "__dcc_dgroup_segment" || name == "__dcc_code_segment")
                    value.kind = ImageSymbolKind::SegmentValue;
                image.symbols.push_back(std::move(value));
            }
            return image.symbols[it->second];
        };
        auto fragment = [&](std::string name, std::string section, ImageRole role) -> ImageFragment& {
            ImageFragment value;
            value.id = image.fragments.size();
            value.name = std::move(name);
            value.section = std::move(section);
            value.role = role;
            value.model = target.code_model;
            value.required_cpu = target.cpu;
            value.source_file_id = module.source_file_id;
            image.fragments.push_back(std::move(value));
            return image.fragments.back();
        };
        auto binding = [](ir::Linkage linkage) {
            return linkage == ir::Linkage::Internal                                         ? ImageBinding::Local
                   : linkage == ir::Linkage::WeakODR || linkage == ir::Linkage::LinkOnceODR ? ImageBinding::Weak
                                                                                            : ImageBinding::Global;
        };
        auto section_role = [](std::string_view section, bool alloc, bool exec, bool nobits, bool write) {
            if (!alloc)
                return ImageRole::Metadata;
            if (section == ".start" || section.starts_with(".start."))
                return ImageRole::Startup;
            if (section == ".stack" || section.starts_with(".stack."))
                return ImageRole::Stack;
            if (exec)
                return ImageRole::Code;
            if (nobits)
                return ImageRole::Bss;
            if (section == ".data.rel.ro" || section.starts_with(".data.rel.ro."))
                return ImageRole::Relro;
            if (section == ".rodata" || section.starts_with(".rodata."))
                return ImageRole::Rodata;
            return write ? ImageRole::Data : ImageRole::Unknown;
        };
        auto width = [](Reloc::Kind kind) -> std::uint64_t {
            return kind == Reloc::Kind::Abs16 || kind == Reloc::Kind::Rel16 ? 2 : kind == Reloc::Kind::Abs64 ? 8 : 4;
        };
        auto fixup = [&](ImageFragment& owner, std::uint64_t offset, std::string_view name, Reloc::Kind kind, std::int64_t addend,
                         std::optional<SymbolId> bound_symbol = std::nullopt) {
            ImageFixup value;
            value.offset = offset;
            value.width = width(kind);
            value.target = bound_symbol ? *bound_symbol : symbol(name).id;
            value.addend = addend;
            value.encoding = kind;
            value.legacy_name = name;
            if (kind == Reloc::Kind::Rel16 || kind == Reloc::Kind::Rel32 || kind == Reloc::Kind::Rel32_Call)
            {
                value.expression = ImageExpression::PcRelative;
                value.space = ImageSpace::Code;
            }
            else if (name == "__dcc_dgroup_segment" || name == "__dcc_code_segment")
            {
                value.expression = ImageExpression::SegmentBase;
                value.units = ImageUnits::Paragraphs;
                value.space = name == "__dcc_code_segment" ? ImageSpace::Code : ImageSpace::Dgroup;
            }
            owner.fixups.push_back(std::move(value));
        };

        for (auto* function : module.functions)
            if (function)
            {
                auto& value = symbol(function->name);
                value.function = function;
                value.kind = ImageSymbolKind::Code;
                value.binding = binding(function->linkage);
            }
        for (auto* global : module.globals)
            if (global)
            {
                auto& value = symbol(global->name);
                value.global = global;
                if (value.kind != ImageSymbolKind::SegmentValue)
                    value.kind = ImageSymbolKind::Data;
                value.binding = binding(global->linkage);
            }
        for (std::size_t index = 0; index < machine.functions.size(); ++index)
        {
            auto const& function = machine.functions[index];
            auto const& code = encoded.at(index);
            auto section = function_section(module, function.name(), target.function_sections);
            if (section.empty())
                section = ".text";
            auto& part = fragment(std::string{function.name()}, section, section_role(section, true, true, false, false));
            part.exec = true;
            part.bytes = code.bytes;
            part.memory_size = code.bytes.size();
            part.source_line = function.src_line;
            auto& definition = symbol(function.name());
            definition.fragment = part.id;
            definition.size = part.memory_size;
            definition.kind = ImageSymbolKind::Code;
            definition.binding = binding(function.linkage);
            part.function = definition.function;
            if (part.function)
                part.source_file_id = part.function->decl_file_id;
            image.functions.push_back(part.id);
            for (auto const& relocation : code.relocs)
                fixup(part, relocation.offset, relocation.symbol, relocation.kind, relocation.addend);
            std::vector<std::pair<std::uint32_t, std::uint32_t>> blocks(code.block_offsets.begin(), code.block_offsets.end());
            std::ranges::sort(blocks);
            for (auto [id, offset] : blocks)
            {
                auto& block = symbol(std::string{function.name()} + ".bb" + std::to_string(id));
                block.fragment = part.id;
                block.offset = offset;
                block.kind = ImageSymbolKind::Code;
                block.binding = ImageBinding::Local;
                block.function = part.function;
                block.block = id;
            }
        }
        for (auto* global : module.globals)
        {
            if (!global || classify_global(global) == DataSection::None)
                continue;
            auto kind = classify_global(global);
            auto section = global_section(global, target.data_sections);
            if (section.empty())
                section = kind == DataSection::Rodata        ? ".rodata"
                          : kind == DataSection::RodataRelRO ? ".data.rel.ro"
                          : kind == DataSection::Bss         ? ".bss"
                                                             : ".data";
            auto& part = fragment(std::string{global->name}, section, section_role(section, true, false, kind == DataSection::Bss, !global->is_constant));
            part.global = global;
            part.retain = global->retain;
            part.write = kind == DataSection::RodataRelRO || !global->is_constant;
            part.nobits = kind == DataSection::Bss;
            part.alignment = global_alignment(global);
            if (section == model_marker_section)
            {
                part.role = ImageRole::Metadata;
                part.alloc = false;
            }
            part.memory_size = global->type ? global->type->byte_size : 0;
            auto& definition = symbol(global->name);
            definition.fragment = part.id;
            definition.size = part.memory_size;
            if (global->init)
            {
                auto size = init_type_size(global->type);
                if (size == 0 && global->init->type)
                    size = init_type_size(global->init->type);
                part.bytes.resize(size);
                part.memory_size = size;
                std::vector<InitReloc> relocations;
                serialize_init_memory(part.bytes, relocations, global->init, global->type, 0, 2);
                for (auto const& relocation : relocations)
                {
                    auto encoding = relocation.size == 2 ? Reloc::Kind::Abs16 : relocation.size == 4 ? Reloc::Kind::Abs32 : Reloc::Kind::Abs64;
                    fixup(part, relocation.offset, relocation.name, encoding, relocation.addend);
                    auto& value = part.fixups.back();
                    value.reference = relocation.reference;
                    if (value.reference)
                        value.range = value.reference->range;
                    if (relocation.segment)
                    {
                        value.target = symbol(relocation.reference->name).id;
                        value.space = relocation.reference->function ? ImageSpace::Code : ImageSpace::Dgroup;
                        value.expression = ImageExpression::SegmentBase;
                        value.units = ImageUnits::Paragraphs;
                    }
                }
            }
            symbol(global->name).size = part.memory_size;
        }
        for (std::size_t index = 0; index < machine.functions.size(); ++index)
        {
            auto const& function = machine.functions[index];
            for (auto const& table : function.jump_tables)
            {
                auto section = target.function_sections || target.data_sections ? ".rodata." + table.symbol : ".rodata";
                auto& part = fragment(table.symbol, section, ImageRole::Rodata);
                part.associated_function = image.functions[index];
                part.function = image.fragments[image.functions[index]].function;
                part.alignment = 2;
                part.bytes.resize(table.targets.size() * 2);
                part.memory_size = part.bytes.size();
                auto& definition = symbol(table.symbol);
                definition.fragment = part.id;
                definition.kind = ImageSymbolKind::Data;
                definition.binding = ImageBinding::Local;
                definition.size = part.memory_size;
                for (std::size_t entry = 0; entry < table.targets.size(); ++entry)
                {
                    fixup(part, entry * 2, std::string{function.name()} + ".bb" + std::to_string(table.targets[entry]), Reloc::Kind::Abs16, 0);
                    part.fixups.back().expression = ImageExpression::CodeOffset;
                    part.fixups.back().space = ImageSpace::Code;
                }
            }
        }
        std::unordered_map<std::string, SymbolId> assembly_locals;
        for (auto const& source : image.assembly.symbols)
            if (source.binding == ModuleAsmBinding::Local)
            {
                ImageSymbol value;
                value.id = image.symbols.size();
                value.name = source.name;
                assembly_locals.emplace(source.name, value.id);
                image.symbols.push_back(std::move(value));
            }
        for (std::size_t index = 0; index < image.assembly.sections.size(); ++index)
        {
            auto const& section = image.assembly.sections[index];
            auto& part = fragment(section.name, section.name, section_role(section.name, section.alloc, section.exec, section.nobits, section.write));
            part.asm_section = index;
            part.alignment = section.alignment;
            part.bytes = section.data;
            part.memory_size = section.nobits ? section.size : section.data.size();
            part.alloc = section.alloc;
            part.write = section.write;
            part.exec = section.exec;
            part.nobits = section.nobits;
            part.opaque = true;
            image.assembly_sections.push_back(part.id);
            for (auto const& relocation : section.relocations)
            {
                auto local = assembly_locals.find(relocation.symbol);
                fixup(part, relocation.offset, relocation.symbol, relocation.kind, relocation.addend,
                      local == assembly_locals.end() ? std::nullopt : std::optional{local->second});
            }
        }
        for (auto const& source : image.assembly.symbols)
        {
            auto& value = source.binding == ModuleAsmBinding::Local ? image.symbols[assembly_locals.at(source.name)] : symbol(source.name);
            value.fragment = image.assembly_sections.at(source.section);
            value.offset = source.offset;
            value.binding = source.binding == ModuleAsmBinding::Local  ? ImageBinding::Local
                            : source.binding == ModuleAsmBinding::Weak ? ImageBinding::Weak
                                                                       : ImageBinding::Global;
            value.kind = source.type == ModuleAsmSymbolType::Function || image.fragments[*value.fragment].exec ? ImageSymbolKind::Code : ImageSymbolKind::Data;
        }
        for (auto& part : image.fragments)
            for (auto& value : part.fixups)
                if (value.space == ImageSpace::Unknown)
                {
                    auto const kind = image.symbols[value.target].kind;
                    value.space = kind == ImageSymbolKind::Code ? ImageSpace::Code : kind == ImageSymbolKind::Data ? ImageSpace::Dgroup : ImageSpace::Unknown;
                    if (kind == ImageSymbolKind::Code)
                        value.expression = ImageExpression::CodeOffset;
                }
        return image;
    }
}
