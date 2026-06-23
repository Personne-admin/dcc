export module dcc.ir.lower;

import std;
import dcc.sm;
import dcc.ast;
import dcc.lex.tokens;
import dcc.types;
import dcc.comptime;
import dcc.ir;
import dcc.ir.mangle;
import dcc.sema.scope;
import dcc.sema.importer;
import dcc.sema.instantiator;

export namespace dcc::ir::lower
{
    class Lowerer
    {
    public:
        explicit Lowerer(IrContext& ctx, sema::SpecializationRegistry const* spec_reg = nullptr, sema::ModuleGraph const* module_graph = nullptr,
                         bool bounds_check = false, sm::SourceManager const* source_manager = nullptr, dcc::types::TypeContext* type_ctx = nullptr)
            : m_ctx(ctx), m_spec_reg(spec_reg), m_module_graph(module_graph), m_type_ctx(type_ctx), m_bounds_check(bounds_check),
              m_source_manager(source_manager)
        {
        }

        IrModule* lower_module(sema::ModuleInfo const& mod)
        {
            m_entry_module = &mod;

            auto segs = mod.canonical_path.segments();
            m_module_path.clear();
            m_module_path.reserve(segs.size());
            for (auto const& s : segs)
                m_module_path.push_back(std::string_view{s});

            {
                auto mod_name_str = mod.canonical_path.str();
                auto* mem = static_cast<char*>(m_ctx.allocator().allocate_bytes(mod_name_str.size(), alignof(char)));
                std::memcpy(mem, mod_name_str.data(), mod_name_str.size());
                m_module = m_ctx.module(std::string_view{mem, mod_name_str.size()});
                if (mod.file_id != sm::FileId::Invalid)
                    m_module->source_file_id = static_cast<std::uint32_t>(mod.file_id);
            }

            build_nominal_resolver();
            lower_globals(mod);
            build_all_function_shells(mod);
            lower_all_function_bodies();

            return m_module;
        }

    private:
        void build_nominal_resolver()
        {
            if (!m_module_graph)
            {
                m_nominal_resolver = [](void const*) -> std::optional<dcc::ir::mangle::NominalInfo> { return std::nullopt; };
                return;
            }

            std::unordered_map<ast::Decl const*, dcc::ir::mangle::NominalInfo> decl_map;

            for (auto const& mod_ptr : m_module_graph->all())
            {
                auto* tu = mod_ptr->tu;
                if (!tu)
                    continue;

                auto mod_path_segs = mod_ptr->canonical_path.segments();
                std::vector<std::string> owned_segs(mod_path_segs.begin(), mod_path_segs.end());

                for (auto* d : tu->decls)
                {
                    std::string_view decl_name;
                    if (auto* sd = ast::node_cast<ast::StructDecl>(d))
                        decl_name = sd->name;
                    else if (auto* ud = ast::node_cast<ast::UnionDecl>(d))
                        decl_name = ud->name;
                    else if (auto* ed = ast::node_cast<ast::EnumDecl>(d))
                        decl_name = ed->name;
                    else if (auto* usingd = ast::node_cast<ast::UsingDecl>(d))
                    {
                        bool is_nominal = false;
                        for (auto const& attr : usingd->attrs)
                            if (attr.name == "nominal")
                            {
                                is_nominal = true;
                                break;
                            }
                        if (!is_nominal)
                            continue;

                        decl_name = usingd->alias_path.tail_name();
                    }
                    else
                        continue;

                    if (decl_name.empty())
                        continue;

                    auto name_buf = std::string(decl_name);
                    auto* name_ptr = m_nominal_names.insert(m_nominal_names.end(), std::move(name_buf))->c_str();

                    std::vector<std::string_view> path_svs;
                    path_svs.reserve(owned_segs.size());
                    for (auto const& seg : owned_segs)
                        path_svs.push_back(seg);

                    dcc::ir::mangle::NominalInfo info;
                    info.name = name_ptr;
                    info.module_path = std::move(path_svs);

                    auto stored_path = std::vector<std::string>(mod_path_segs.begin(), mod_path_segs.end());
                    auto* stored_path_ptr = &*m_nominal_paths.insert(m_nominal_paths.end(), std::move(stored_path));

                    std::vector<std::string_view> stable_path;
                    stable_path.reserve(stored_path_ptr->size());
                    for (auto const& seg : *stored_path_ptr)
                        stable_path.push_back(seg);

                    info.module_path = std::move(stable_path);
                    decl_map[d] = std::move(info);
                }
            }

            m_nominal_resolver = [decl_map = std::move(decl_map)](void const* ptr) -> std::optional<dcc::ir::mangle::NominalInfo> {
                auto* decl = static_cast<ast::Decl const*>(ptr);
                auto it = decl_map.find(decl);
                if (it != decl_map.end())
                    return it->second;

                return std::nullopt;
            };
        }

        static bool is_template_func(ast::FuncDecl const* fd) { return fd && !fd->template_params.empty(); }

        [[nodiscard]] sema::ModuleInfo const* owning_module_of(ast::Decl const* decl) const
        {
            if (!decl || !m_module_graph)
                return m_entry_module;

            auto fid = decl->range.begin.fileId;
            if (fid == sm::FileId::Invalid)
                return m_entry_module;

            for (auto const& mod_ptr : m_module_graph->all())
                if (mod_ptr && mod_ptr->file_id == fid)
                    return mod_ptr.get();

            return m_entry_module;
        }

        [[nodiscard]] std::span<std::string_view const> module_path_for_decl(ast::Decl const* decl)
        {
            auto* owning = owning_module_of(decl);
            if (!owning || owning == m_entry_module)
                return m_module_path;

            m_module_path_cache.clear();
            auto segs = owning->canonical_path.segments();
            for (auto const& s : segs)
                m_module_path_cache.push_back(std::string_view{s});
            return m_module_path_cache;
        }

        void build_all_function_shells(sema::ModuleInfo const& mod)
        {
            if (mod.tu)
                for (auto* d : mod.tu->decls)
                    if (auto* fd = ast::node_cast<ast::FuncDecl>(d))
                        if (!is_template_func(fd))
                            create_func_shell(fd, true);

            if (m_spec_reg)
            {
                auto specs = m_spec_reg->entries();

                std::ranges::sort(specs, [](auto const& a, auto const& b) {
                    auto a_name = a.template_decl ? a.template_decl->name : std::string_view{};
                    auto b_name = b.template_decl ? b.template_decl->name : std::string_view{};
                    if (a_name != b_name)
                        return a_name < b_name;

                    auto a_size = a.canonical_args.size();
                    auto b_size = b.canonical_args.size();
                    if (a_size != b_size)
                        return a_size < b_size;

                    for (std::size_t i = 0; i < a_size; ++i)
                    {
                        auto const& ac = a.canonical_args[i];
                        auto const& bc = b.canonical_args[i];
                        if (ac.tag != bc.tag)
                            return static_cast<int>(ac.tag) < static_cast<int>(bc.tag);

                        if (ac.type_ptr != bc.type_ptr)
                            return ac.type_ptr < bc.type_ptr;

                        if (ac.value_hash != bc.value_hash)
                            return ac.value_hash < bc.value_hash;
                    }
                    return false;
                });

                for (auto const& spec : specs)
                {
                    if (spec.specialization_decl)
                        create_specialization_shell(spec);
                }
            }
        }

        void lower_all_function_bodies()
        {
            std::vector<ast::FuncDecl const*> to_lower;
            to_lower.reserve(m_func_map.size());
            for (auto& [fd, ir_func] : m_func_map)
            {
                if (!fd->body || is_template_func(fd))
                    continue;
                if (ir_func->linkage == Linkage::External && !m_definition_functions.contains(fd))
                    continue;
                to_lower.push_back(fd);
            }

            std::ranges::sort(to_lower, [](ast::FuncDecl const* a, ast::FuncDecl const* b) {
                auto const a_ok = a->range.begin.valid();
                auto const b_ok = b->range.begin.valid();
                if (a_ok != b_ok)
                    return a_ok > b_ok;
                if (!a_ok)
                    return a->name < b->name;
                auto a_file = static_cast<std::uint32_t>(a->range.begin.fileId);
                auto b_file = static_cast<std::uint32_t>(b->range.begin.fileId);
                if (a_file != b_file)
                    return a_file < b_file;
                return a->range.begin.offset < b->range.begin.offset;
            });

            for (auto* fd : to_lower)
            {
                auto it = m_func_map.find(fd);
                if (it == m_func_map.end())
                    lower_panic(std::format("function `{}` missing from func_map during body lowering", fd->name));
                lower_func_body(fd, it->second);
            }
        }

        void create_func_shell(ast::FuncDecl const* decl, bool is_definition_here)
        {
            if (m_func_map.find(decl) != m_func_map.end())
                return;

            if (is_template_func(decl))
                return;

            auto module_path = is_definition_here ? std::span<std::string_view const>{m_module_path} : module_path_for_decl(decl);

            auto* ret_canonical = get_canonical_type(decl->return_type);
            std::vector<dcc::types::TypePtr> param_canonical;
            param_canonical.reserve(decl->params.size());
            for (auto const& p : decl->params)
                param_canonical.push_back(get_canonical_type(p.type));

            m_name_pool.push_back(dcc::ir::mangle::mangle_function(module_path, *decl, param_canonical, ret_canonical, {}, m_nominal_resolver));
            std::string_view mangled_name = m_name_pool.back();

            auto* ir_ret_type = lower_type(ret_canonical);
            std::vector<IrType const*> ir_param_types;
            ir_param_types.reserve(decl->params.size());
            for (auto* ct : param_canonical)
                ir_param_types.push_back(lower_type(ct));

            auto* func_type = dcc::ir::ir_type_cast<dcc::ir::IrFuncType>(m_ctx.func_t(ir_ret_type, ir_param_types));
            auto* ir_func = m_ctx.function(mangled_name, func_type);

            ir_func->linkage = Linkage::External;

            if (is_definition_here)
                m_definition_functions.insert(decl);

            ir_func->source_name = decl->name;
            if (decl->range.valid() && m_source_manager)
            {
                ir_func->decl_file_id = static_cast<std::uint32_t>(decl->range.begin.fileId);
                auto lc = m_source_manager->line_col(decl->range.begin);
                if (lc)
                    ir_func->decl_line = lc->line;
            }

            propagate_attrs(decl, ir_func);

            m_func_map[decl] = ir_func;
            m_module->functions.push_back(ir_func);
        }

        void create_specialization_shell(sema::SpecializationView const& spec)
        {
            auto* fd = spec.specialization_decl;
            if (!fd)
                return;

            if (fd->sema.is_intrinsic)
                return;

            if (m_func_map.find(fd) != m_func_map.end())
                return;

            auto module_path = module_path_for_decl(spec.template_decl ? static_cast<ast::Decl const*>(spec.template_decl) : nullptr);
            if (module_path.empty())
                module_path = std::span<std::string_view const>{m_module_path};

            auto* ret_canonical = get_canonical_type(fd->return_type);
            std::vector<dcc::types::TypePtr> param_canonical;
            param_canonical.reserve(fd->params.size());
            for (auto const& p : fd->params)
                param_canonical.push_back(get_canonical_type(p.type));

            std::vector<dcc::ir::mangle::TemplateArg> template_args;
            template_args.reserve(spec.canonical_args.size());
            for (auto const& ca : spec.canonical_args)
                template_args.push_back(canonical_to_template_arg(ca));

            auto templ_name = spec.template_decl ? spec.template_decl->name : std::string_view{};
            m_name_pool.push_back(
                dcc::ir::mangle::mangle_specialization(module_path, templ_name, param_canonical, ret_canonical, template_args, m_nominal_resolver));
            std::string_view mangled_name = m_name_pool.back();

            auto* ir_ret_type = lower_type(ret_canonical);
            std::vector<IrType const*> ir_param_types;
            ir_param_types.reserve(fd->params.size());
            for (auto* ct : param_canonical)
                ir_param_types.push_back(lower_type(ct));

            auto* func_type = dcc::ir::ir_type_cast<dcc::ir::IrFuncType>(m_ctx.func_t(ir_ret_type, ir_param_types));
            auto* ir_func = m_ctx.function(mangled_name, func_type);

            ir_func->linkage = Linkage::LinkOnceODR;

            m_definition_functions.insert(fd);

            ir_func->source_name = fd->name;
            if (fd->range.valid() && m_source_manager)
            {
                ir_func->decl_file_id = static_cast<std::uint32_t>(fd->range.begin.fileId);
                auto lc = m_source_manager->line_col(fd->range.begin);
                if (lc)
                    ir_func->decl_line = lc->line;
            }

            propagate_attrs(fd, ir_func);

            m_func_map[fd] = ir_func;
            m_module->functions.push_back(ir_func);
        }

        [[nodiscard]] IrFunction* get_or_create_func_ref(ast::FuncDecl const* fd)
        {
            if (!fd)
                lower_panic("get_or_create_func_ref on null FuncDecl");

            if (is_template_func(fd))
            {
                auto it = m_func_map.find(fd);
                if (it != m_func_map.end())
                    return it->second;
                lower_panic(std::format("template function `{}` not found in function map (specialization not instantiated?)", fd->name));
            }

            auto it = m_func_map.find(fd);
            if (it != m_func_map.end())
                return it->second;

            auto* owning = owning_module_of(fd);
            if (owning == m_entry_module || !owning)
            {
                if (owning == m_entry_module)
                    create_func_shell(fd, true);
                else
                    return nullptr;
            }
            else
                create_func_shell(fd, false);

            it = m_func_map.find(fd);
            if (it != m_func_map.end())
                return it->second;

            lower_panic(std::format("failed to create function shell for `{}`", fd->name));
        }

        [[nodiscard]] IrGlobal* get_or_create_global_ref(ast::VarDecl const* vd)
        {
            if (!vd)
                return nullptr;

            auto it = m_global_map.find(vd);
            if (it != m_global_map.end())
                return it->second;

            auto storage = vd->sema.storage;
            if (storage != ast::StorageClass::ModuleGlobal && storage != ast::StorageClass::Static && storage != ast::StorageClass::Extern)
                return nullptr;

            auto* owning = owning_module_of(vd);
            if (!owning || owning == m_entry_module)
                return nullptr;

            auto module_path = module_path_for_decl(vd);
            auto* ir_type = lower_type(get_canonical_type(vd->type));
            auto mangled = dcc::ir::mangle::mangle_global(module_path, *vd, get_canonical_type(vd->type), {}, m_nominal_resolver);
            m_name_pool.push_back(std::move(mangled));
            auto name_sv = std::string_view{m_name_pool.back()};
            auto* ir_global = m_ctx.global(name_sv, ir_type, nullptr, false);
            ir_global->linkage = Linkage::External;
            ir_global->is_dll_import = vd->sema.is_dll_import;
            ir_global->is_dll_export = vd->sema.is_dll_export;
            ir_global->alignment = vd->sema.alignment;
            if (!vd->sema.section.empty())
                ir_global->section = vd->sema.section;

            m_global_map[vd] = ir_global;
            m_module->globals.push_back(ir_global);
            return ir_global;
        }

        static dcc::ir::mangle::TemplateArg canonical_to_template_arg(sema::CanonicalArg const& ca)
        {
            dcc::ir::mangle::TemplateArg ta;
            if (ca.tag == sema::CanonicalArg::Tag::Type)
            {
                ta.kind = dcc::ir::mangle::TemplateArg::Kind::Type;
                ta.type = ca.type_ptr;
            }
            else
            {
                ta.kind = dcc::ir::mangle::TemplateArg::Kind::Value;
                if (ca.value_data)
                    ta.value = ca.value_data.get();
                else
                    ta.value = nullptr;
            }
            return ta;
        }

        void propagate_attrs(ast::FuncDecl const* decl, IrFunction* ir_func)
        {
            auto const& sema = decl->sema;

            if (!sema.calling_conv.empty())
            {
                std::string_view cc = sema.calling_conv;

                auto cc_enum = calling_conv_from_string(cc);
                ir_func->attrs.push_back({IrFuncAttr::CallingConv, cc});
                std::ignore = cc_enum;
            }

            if (sema.is_inline)
                ir_func->attrs.push_back({IrFuncAttr::Inline, {}});
            if (sema.is_noinline)
                ir_func->attrs.push_back({IrFuncAttr::NoInline, {}});

            if (sema.is_nomangle)
                ir_func->attrs.push_back({IrFuncAttr::NoMangle, {}});

            if (!sema.section.empty())
                ir_func->attrs.push_back({IrFuncAttr::Section, sema.section});

            ir_func->is_dll_import = sema.is_dll_import;
            ir_func->is_dll_export = sema.is_dll_export;

            ir_func->alignment = sema.alignment;
        }

        static dcc::ir::CallingConv calling_conv_from_string(std::string_view s)
        {
            auto eq = [](std::string_view a, std::string_view b) {
                return std::ranges::equal(
                    a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
            };

            if (eq(s, "cdecl") || s.empty())
                return dcc::ir::CallingConv::Cdecl;
            if (eq(s, "stdcall"))
                return dcc::ir::CallingConv::Stdcall;
            if (eq(s, "fastcall"))
                return dcc::ir::CallingConv::Fastcall;
            if (eq(s, "vectorcall"))
                return dcc::ir::CallingConv::Vectorcall;
            if (eq(s, "systemv") || eq(s, "sysv"))
                return dcc::ir::CallingConv::SystemV;
            if (eq(s, "win64"))
                return dcc::ir::CallingConv::Win64;

            return dcc::ir::CallingConv::Cdecl;
        }

        void lower_func_body(ast::FuncDecl const* decl, IrFunction* ir_func)
        {
            SourceRangeGuard guard(*this, decl->range);

            m_value_map.clear();
            m_named_values.clear();
            m_next_local_id = 0;
            m_next_block_id = 1;
            m_next_scope_id = 0;
            m_current_func = ir_func;
            m_current_func_decl = decl;
            m_current_block = nullptr;
            m_scope_frames.clear();
            m_loop_stack.clear();

            auto* ret_canonical = get_canonical_type(decl->return_type);
            std::vector<dcc::types::TypePtr> param_canonical;
            param_canonical.reserve(decl->params.size());
            for (auto const& p : decl->params)
                param_canonical.push_back(get_canonical_type(p.type));

            auto* ir_ret_type = lower_type(ret_canonical);
            std::vector<IrType const*> ir_param_types;
            ir_param_types.reserve(decl->params.size());
            for (auto* ct : param_canonical)
                ir_param_types.push_back(lower_type(ct));

            auto* entry = m_ctx.basic_block("entry", 0);
            m_current_block = entry;
            ir_func->entry_block = entry;
            ir_func->blocks.push_back(entry);

            for (std::size_t i = 0; i < decl->params.size(); ++i)
            {
                auto& p = decl->params[i];
                auto* local = m_ctx.local(p.name, static_cast<std::uint32_t>(i), ir_param_types[i]);
                entry->params.push_back(local);

                auto* syn_decl = p.synthetic_decl;
                if (!syn_decl)
                    lower_panic(decl, std::format("parameter `{}` missing synthetic_decl", p.name));

                bool is_aggregate = false;

                if (ir_param_types[i])
                {
                    if (ir_param_types[i]->kind == IrTypeKind::Aggregate || ir_param_types[i]->kind == IrTypeKind::Array ||
                        ir_param_types[i]->kind == IrTypeKind::Slice)
                        is_aggregate = true;
                }

                if (!is_aggregate && param_canonical[i])
                {
                    auto const sk = param_canonical[i]->kind;
                    if (sk == types::TypeKind::Struct || sk == types::TypeKind::Union || sk == types::TypeKind::Array || sk == types::TypeKind::Slice)
                        is_aggregate = true;
                }

                if (is_aggregate)
                {
                    auto* ptr_type = m_ctx.pointer_to(ir_param_types[i], ir::Segment::None);
                    auto* alloca = m_ctx.alloca(ptr_type, ir_param_types[i]);

                    std::uint32_t align = 0;
                    if (syn_decl->sema.alignment != 0)
                        align = syn_decl->sema.alignment;
                    else if (syn_decl->sema.byte_align != 0)
                        align = syn_decl->sema.byte_align;
                    else if (p.sema.alignment != 0)
                        align = p.sema.alignment;
                    else if (p.sema.byte_align != 0)
                        align = p.sema.byte_align;

                    if (align != 0)
                        alloca->alignment = align;

                    auto name = ident_name();
                    alloca->name = m_name_pool.back();
                    append_inst(alloca);

                    bool is_volatile = false;
                    if (p.type)
                    {
                        if (auto* qt = ast::node_cast<ast::QualifiedType>(p.type))
                            is_volatile = ast::has_qual(qt->quals, ast::Qual::Volatile);
                    }

                    if (is_volatile)
                        append_inst(m_ctx.store_volatile(local, alloca));
                    else
                        append_inst(m_ctx.store(local, alloca));

                    m_value_map[syn_decl] = MapEntry{alloca, true, param_canonical[i], is_volatile};
                }
                else
                {
                    m_value_map[syn_decl] = MapEntry{local, false, param_canonical[i], false};
                }
            }

            if (decl->body)
                lower_block(*decl->body);

            if (!current_block_terminated())
                if (ir_ret_type->kind == IrTypeKind::Void)
                    emit_ret();
        }

        IrBasicBlock* create_block(std::string_view name = {})
        {
            auto id = m_next_block_id++;
            IrBasicBlock* bb;
            if (name.empty())
                bb = m_ctx.basic_block(id);
            else
            {
                auto stamp = std::to_string(id);
                m_name_pool.push_back(std::string{name} + stamp);
                bb = m_ctx.basic_block(m_name_pool.back(), id);
            }

            bb->parent = m_current_func;
            m_current_func->blocks.push_back(bb);
            return bb;
        }

        void set_current_block(IrBasicBlock* bb) { m_current_block = bb; }

        [[nodiscard]] bool current_block_terminated() const { return !m_current_block || m_current_block->terminator != nullptr; }

        void emit_br(IrBasicBlock* target)
        {
            if (current_block_terminated())
                return;

            m_current_block->terminator = m_ctx.br(target);
            record_terminator_source_loc();
        }

        void emit_br_cond(IrValue* cond, IrBasicBlock* true_target, IrBasicBlock* false_target)
        {
            if (current_block_terminated())
                return;

            m_current_block->terminator = m_ctx.br_cond(cond, true_target, false_target);
            record_terminator_source_loc();
        }

        void emit_ret(IrValue* val = nullptr)
        {
            if (current_block_terminated())
                return;

            m_current_block->terminator = m_ctx.ret(val);
            record_terminator_source_loc();
        }

        void emit_unreachable()
        {
            if (current_block_terminated())
                return;

            m_current_block->terminator = m_ctx.unreachable();
            record_terminator_source_loc();
        }

        void emit_switch(IrValue* value, IrBasicBlock* default_target)
        {
            if (current_block_terminated())
                return;

            m_current_block->terminator = m_ctx.switch_(value, default_target);
            record_terminator_source_loc();
        }

        IrPhiInst* emit_phi(IrType const* type)
        {
            auto* phi = m_ctx.phi(type);
            auto name = ident_name();
            phi->name = m_name_pool.back();

            auto& insts = m_current_block->instructions;
            auto it = insts.begin();
            while (it != insts.end() && (*it)->kind == IrNodeKind::Phi)
                ++it;

            insert_inst(it, phi);
            return phi;
        }

        void add_phi_incoming(IrPhiInst* phi, IrValue* value, IrBasicBlock* block) { phi->incoming.push_back({value, block}); }

        struct DeferEntry
        {
            ast::Stmt const* body;
        };

        struct ScopeFrame
        {
            std::uint32_t scope_id{};
            std::pmr::vector<DeferEntry> defers;

            explicit ScopeFrame(std::pmr::polymorphic_allocator<> a) : defers(a) {}
        };

        void push_scope()
        {
            m_scope_frames.push_back(ScopeFrame(m_ctx.allocator()));
            m_scope_frames.back().scope_id = ++m_next_scope_id;
        }

        void pop_scope()
        {
            if (m_scope_frames.empty())
                return;

            if (!current_block_terminated())
            {
                auto& defers = m_scope_frames.back().defers;
                for (auto it = defers.rbegin(); it != defers.rend(); ++it)
                    lower_stmt(it->body);
            }

            m_scope_frames.pop_back();
        }

        [[nodiscard]] std::uint32_t current_scope_id() const noexcept { return m_scope_frames.empty() ? 0u : m_scope_frames.back().scope_id; }

        struct SourceRangeGuard
        {
            Lowerer& lowerer;
            sm::SourceRange prev;

            explicit SourceRangeGuard(Lowerer& l, sm::SourceRange r) : lowerer(l), prev(l.m_active_range) { lowerer.m_active_range = r; }
            ~SourceRangeGuard() { lowerer.m_active_range = prev; }

            SourceRangeGuard(SourceRangeGuard const&) = delete;
            SourceRangeGuard& operator=(SourceRangeGuard const&) = delete;
        };

        [[nodiscard]] SourceLoc make_source_loc(sm::SourceRange range, std::uint32_t scope_id) const
        {
            SourceLoc sl{};
            if (!range.valid() || !m_source_manager)
            {
                sl.file_id = static_cast<std::uint32_t>(sm::FileId::Invalid);
                sl.line = 0;
                sl.column = 0;
            }
            else
            {
                sl.file_id = static_cast<std::uint32_t>(range.begin.fileId);
                auto lc = m_source_manager->line_col(range.begin);
                if (lc)
                {
                    sl.line = lc->line;
                    sl.column = lc->column;
                }
                else
                {
                    sl.line = 0;
                    sl.column = 0;
                }
            }

            sl.scope_id = scope_id;
            return sl;
        }

        void append_inst(IrValue* inst)
        {
            auto const scope = current_scope_id();
            IrDebugLocation rec{};
            rec.block_id = m_current_block->id;
            rec.instruction_index = static_cast<std::uint32_t>(m_current_block->instructions.size());
            rec.is_terminator = false;
            rec.loc = make_source_loc(m_active_range, scope);
            m_current_block->instructions.push_back(inst);
            m_current_func->debug_locations.push_back(rec);
        }

        void insert_inst(std::pmr::vector<IrValue*>::iterator it, IrValue* inst)
        {
            auto const idx = static_cast<std::uint32_t>(it - m_current_block->instructions.begin());
            auto const block_id = m_current_block->id;

            for (auto& dl : m_current_func->debug_locations)
            {
                if (dl.block_id == block_id && !dl.is_terminator && dl.instruction_index >= idx)
                    ++dl.instruction_index;
            }

            auto const scope = current_scope_id();
            IrDebugLocation rec{};
            rec.block_id = block_id;
            rec.instruction_index = idx;
            rec.is_terminator = false;
            rec.loc = make_source_loc(m_active_range, scope);
            m_current_block->instructions.insert(it, inst);
            m_current_func->debug_locations.push_back(rec);
        }

        void record_terminator_source_loc()
        {
            auto const scope = current_scope_id();
            IrDebugLocation rec{};
            rec.block_id = m_current_block->id;
            rec.instruction_index = static_cast<std::uint32_t>(m_current_block->instructions.size());
            rec.is_terminator = true;
            rec.loc = make_source_loc(m_active_range, scope);
            m_current_func->debug_locations.push_back(rec);
        }

        void register_defer(ast::Stmt const* body)
        {
            if (m_scope_frames.empty())
                lower_panic("defer outside any scope");

            m_scope_frames.back().defers.push_back({body});
        }

        void flush_defers_to_depth(std::size_t depth)
        {
            for (std::size_t i = m_scope_frames.size(); i > depth; --i)
            {
                auto& defers = m_scope_frames[i - 1].defers;
                for (auto it = defers.rbegin(); it != defers.rend(); ++it)
                    lower_stmt(it->body);
            }
        }

        void flush_all_defers() { flush_defers_to_depth(0); }

        struct LoopFrame
        {
            IrBasicBlock* continue_target;
            IrBasicBlock* exit_target;
            std::size_t scope_depth;
        };

        void push_loop(IrBasicBlock* cont, IrBasicBlock* exit) { m_loop_stack.push_back({cont, exit, m_scope_frames.size()}); }

        void pop_loop()
        {
            if (m_loop_stack.empty())
                lower_panic("pop_loop with empty loop stack");

            m_loop_stack.pop_back();
        }

    private:
        struct MapEntry
        {
            IrValue* value{};
            bool is_storage{};
            dcc::types::TypePtr sema_type{};
            bool is_volatile{};
        };

        [[nodiscard]] dcc::types::TypePtr make_ptr_sema_type(dcc::types::TypePtr pointee, dcc::types::Qual quals) const
        {
            if (!m_type_ctx || !pointee)
                return pointee;
            return m_type_ctx->pointer_to(pointee, quals);
        }

        [[nodiscard]] static dcc::types::Qual get_sema_pointee_quals(dcc::types::TypePtr ty)
        {
            if (!ty)
                return dcc::types::Qual::None;
            if (auto const* pt = dcc::types::type_cast<dcc::types::PointerType>(ty))
                return pt->pointee_quals;
            if (auto const* st = dcc::types::type_cast<dcc::types::SliceType>(ty))
                return st->element_quals;
            return dcc::types::Qual::None;
        }

        static std::string fmt_loc(sm::SourceRange range)
        {
            if (!range.valid())
                return {};

            return std::format(" at file={},offset={}", static_cast<std::uint32_t>(range.begin.fileId), range.begin.offset);
        }

        static std::string fmt_ctx(ast::Decl const* decl)
        {
            if (!decl)
                return {};

            std::string s = std::format(" decl_kind={}", static_cast<int>(decl->kind));
            s += fmt_loc(decl->range);
            if (auto* fd = ast::node_cast<ast::FuncDecl>(decl))
                s += std::format(" name=\"{}\"", fd->name);

            return s;
        }

        static std::string fmt_ctx(ast::Expr const* expr)
        {
            if (!expr)
                return {};

            std::string s = std::format(" expr_kind={}", static_cast<int>(expr->kind));
            s += fmt_loc(expr->range);
            if (auto* id = ast::node_cast<ast::IdentExpr>(expr))
                s += std::format(" name=\"{}\"", id->name);

            return s;
        }

        static std::string fmt_ctx(ast::Stmt const* stmt)
        {
            if (!stmt)
                return {};

            std::string s = std::format(" stmt_kind={}", static_cast<int>(stmt->kind));
            s += fmt_loc(stmt->range);
            return s;
        }

        [[noreturn]] static void lower_panic(std::string_view msg)
        {
            std::println(std::cerr, "IR lowerer panic: {}", msg);
            std::abort();
        }

        [[noreturn]] static void lower_panic(ast::Decl const* decl, std::string_view msg)
        {
            std::println(std::cerr, "IR lowerer panic: {}{}", msg, fmt_ctx(decl));
            std::abort();
        }

        [[noreturn]] static void lower_panic(ast::Expr const* expr, std::string_view msg)
        {
            std::println(std::cerr, "IR lowerer panic: {}{}", msg, fmt_ctx(expr));
            std::abort();
        }

        [[noreturn]] static void lower_panic(ast::Stmt const* stmt, std::string_view msg)
        {
            std::println(std::cerr, "IR lowerer panic: {}{}", msg, fmt_ctx(stmt));
            std::abort();
        }

        [[noreturn]] static void lower_unimplemented(ast::Decl const* decl, std::string_view feature)
        {
            std::println(std::cerr, "IR lowerer unimplemented: {}{}", feature, fmt_ctx(decl));
            std::abort();
        }

        [[noreturn]] static void lower_unimplemented(ast::Expr const* expr, std::string_view feature)
        {
            std::println(std::cerr, "IR lowerer unimplemented: {}{}", feature, fmt_ctx(expr));
            std::abort();
        }

        [[noreturn]] static void lower_unimplemented(ast::Stmt const* stmt, std::string_view feature)
        {
            std::println(std::cerr, "IR lowerer unimplemented: {}{}", feature, fmt_ctx(stmt));
            std::abort();
        }

        [[noreturn]] static void lower_unimplemented(sm::SourceRange range, std::string_view feature)
        {
            std::println(std::cerr, "IR lowerer unimplemented: {}{}", feature, fmt_loc(range));
            std::abort();
        }

        static dcc::types::TypePtr get_canonical_type(ast::TypeExpr const* type_expr)
        {
            if (!type_expr)
                return nullptr;

            return reinterpret_cast<dcc::types::TypePtr>(type_expr->sema.canonical);
        }

        static dcc::types::TypePtr get_sema_resolved_type(ast::Expr const* expr)
        {
            if (!expr)
                return nullptr;

            return reinterpret_cast<dcc::types::Type const*>(expr->sema.resolved_type);
        }

        std::string ident_name()
        {
            auto s = std::to_string(m_next_local_id++);
            m_name_pool.push_back(s);
            return m_name_pool.back();
        }

        IrType const* lower_type(dcc::types::TypePtr type)
        {
            if (!type)
                return m_ctx.void_t();

            if (type->kind == dcc::types::TypeKind::Struct || type->kind == dcc::types::TypeKind::Union || type->kind == dcc::types::TypeKind::Enum)
                return lower_user_type(type);

            if (auto* nt = dcc::types::type_cast<dcc::types::NominalType>(type))
                return lower_type(nt->underlying);

            if (auto* it = dcc::types::type_cast<dcc::types::IntType>(type))
                return m_ctx.int_t(it->bits, it->is_signed, it->is_pointer_sized);

            if (auto* ft = dcc::types::type_cast<dcc::types::FloatType>(type))
                return m_ctx.float_t(ft->bits);

            if (type->kind == dcc::types::TypeKind::Bool)
                return m_ctx.bool_t();

            if (type->kind == dcc::types::TypeKind::Char)
                return m_ctx.int_t(8, false);

            if (type->kind == dcc::types::TypeKind::Void)
                return m_ctx.void_t();

            if (type->kind == dcc::types::TypeKind::NullT)
                return m_ctx.pointer_to(m_ctx.void_t(), ir::Segment::None);

            if (auto* pt = dcc::types::type_cast<dcc::types::PointerType>(type))
            {
                auto* ir_pointee = lower_type(pt->pointee);
                return m_ctx.pointer_to(ir_pointee, ir::Segment::None);
            }

            if (auto* fpt = dcc::types::type_cast<dcc::types::FuncPtrType>(type))
            {
                auto* ir_ret = lower_type(fpt->return_type);
                std::vector<IrType const*> ir_params;
                ir_params.reserve(fpt->params.size());
                for (auto* p : fpt->params)
                    ir_params.push_back(lower_type(p));

                auto* ir_func = m_ctx.func_t(ir_ret, ir_params);
                return m_ctx.pointer_to(ir_func, ir::Segment::None);
            }

            if (auto* st = dcc::types::type_cast<dcc::types::SliceType>(type))
            {
                auto* ir_el = lower_type(st->element);
                return m_ctx.slice_t(ir_el, ir::Segment::None);
            }

            if (auto* at = dcc::types::type_cast<dcc::types::ArrayType>(type))
            {
                auto* ir_el = lower_type(at->element);
                return m_ctx.array_t(ir_el, at->count);
            }

            if (auto* rt = dcc::types::type_cast<dcc::types::RuntimeArrayType>(type))
            {
                auto* ir_el = lower_type(rt->element);
                return m_ctx.array_t(ir_el, 0);
            }

            if (auto* ft = dcc::types::type_cast<dcc::types::FamType>(type))
            {
                auto* ir_el = lower_type(ft->element);
                return m_ctx.array_t(ir_el, 0);
            }

            if (dcc::types::type_cast<dcc::types::RangeType>(type) || dcc::types::type_cast<dcc::types::RangeInclusiveType>(type))
                lower_panic("RangeType/RangeInclusiveType must not reach IR type lowering");

            if (type->kind == dcc::types::TypeKind::TemplateParam)
            {
                auto* tp = static_cast<dcc::types::TemplateParamType const*>(type);
                std::string msg = std::format("TemplateParam '{}' (index {}) reached lower_type: sema failed to substitute (see enum construction)", tp->name, tp->index);
                lower_panic(msg);
            }

            std::string reason = std::format("unsupported type kind: {}", static_cast<int>(type->kind));
            lower_panic(reason);
        }

        IrValue* materialize_value(dcc::comptime::Value const& cv)
        {
            switch (cv.kind())
            {
                case dcc::comptime::Value::Kind::Int:
                    return m_ctx.int_const(lower_type(cv.type), cv.get_int());
                case dcc::comptime::Value::Kind::Float:
                    return m_ctx.float_const(lower_type(cv.type), cv.get_float());
                case dcc::comptime::Value::Kind::Bool:
                    return m_ctx.bool_const(cv.get_bool());
                case dcc::comptime::Value::Kind::Char:
                    return m_ctx.int_const(m_ctx.int_t(8, false), static_cast<std::int64_t>(cv.get_char()));
                case dcc::comptime::Value::Kind::Null:
                    return m_ctx.null_const(lower_type(cv.type));
                default:
                    lower_panic("non-scalar comptime value materialization not supported");
            }
        }

        IrValue* lower_block_body(ast::Block const& block)
        {
            SourceRangeGuard guard(*this, block.range);
            push_scope();
            for (auto* stmt : block.stmts)
                lower_stmt(stmt);

            IrValue* tail_val = nullptr;
            if (block.tail && !current_block_terminated())
                tail_val = lower_expr(block.tail);

            pop_scope();
            return tail_val;
        }

        void lower_block(ast::Block const& block)
        {
            lower_block_body(block);

            if (!current_block_terminated())
            {
                auto* ret_type = m_current_func ? m_current_func->func_type->return_type : nullptr;
                if (ret_type && ret_type->kind == IrTypeKind::Void)
                    emit_ret();
            }
        }

        void lower_return_stmt(ast::ReturnStmt const* rs)
        {
            IrValue* val = nullptr;
            if (rs->value)
                val = lower_implicit_enum_construction(rs->value, [&]() { return lower_expr(rs->value); });

            if (val && m_current_func_decl)
            {
                auto* ret_sema_type = get_canonical_type(m_current_func_decl->return_type);
                auto* val_sema_type = get_sema_resolved_type(rs->value);
                if (ret_sema_type && ret_sema_type->kind == types::TypeKind::Slice && val_sema_type && val_sema_type->kind == types::TypeKind::Array)
                    val = coerce_array_to_slice(val, val_sema_type, ret_sema_type);
            }

            flush_all_defers();

            if (val)
                emit_ret(val);
            else
                emit_ret();
        }

        void lower_while_stmt(ast::WhileStmt const* ws)
        {
            auto* cond_bb = create_block("while.cond");
            auto* body_bb = create_block("while.body");
            auto* exit_bb = create_block("while.exit");

            emit_br(cond_bb);

            set_current_block(cond_bb);
            auto* cond_val = lower_expr(ws->condition);
            emit_br_cond(cond_val, body_bb, exit_bb);

            set_current_block(body_bb);
            push_loop(cond_bb, exit_bb);
            lower_block_body(ws->body);
            pop_loop();

            emit_br(cond_bb);

            set_current_block(exit_bb);
        }

        void lower_dowhile_stmt(ast::DoWhileStmt const* dws)
        {
            auto* body_bb = create_block("dowhile.body");
            auto* cond_bb = create_block("dowhile.cond");
            auto* exit_bb = create_block("dowhile.exit");

            emit_br(body_bb);

            set_current_block(body_bb);
            push_loop(cond_bb, exit_bb);
            lower_block_body(dws->body);
            pop_loop();

            emit_br(cond_bb);

            set_current_block(cond_bb);
            auto* cond_val = lower_expr(dws->condition);
            emit_br_cond(cond_val, body_bb, exit_bb);

            set_current_block(exit_bb);
        }

        void lower_for_stmt(ast::ForStmt const* fs)
        {
            if (fs->init)
                lower_stmt(fs->init);

            auto* cond_bb = create_block("for.cond");
            auto* body_bb = create_block("for.body");
            auto* step_bb = create_block("for.step");
            auto* exit_bb = create_block("for.exit");

            emit_br(cond_bb);

            set_current_block(cond_bb);
            if (fs->cond)
            {
                auto* cond_val = lower_expr(fs->cond);
                emit_br_cond(cond_val, body_bb, exit_bb);
            }
            else
                emit_br(body_bb);

            set_current_block(body_bb);
            push_loop(step_bb, exit_bb);
            lower_block_body(fs->body);
            pop_loop();
            emit_br(step_bb);

            set_current_block(step_bb);
            if (fs->update)
                lower_expr(fs->update);

            emit_br(cond_bb);

            set_current_block(exit_bb);
        }

        void lower_forin_stmt(ast::ForInStmt const* fs)
        {
            auto* iter_expr = fs->iterable;
            if (!iter_expr)
                lower_panic(fs, "for-in missing iterable");

            auto* iter_sema_type = get_sema_resolved_type(iter_expr);
            if (!iter_sema_type)
                lower_panic(fs, "for-in iterable missing type");

            dcc::types::TypePtr element_sema_type = nullptr;
            dcc::types::Qual element_quals = dcc::types::Qual::None;

            bool is_range = false;
            bool is_inclusive = false;
            if (auto const* rt = types::type_cast<types::RangeType>(iter_sema_type))
            {
                element_sema_type = rt->element;
                is_range = true;
                is_inclusive = false;
            }
            else if (auto const* rit = types::type_cast<types::RangeInclusiveType>(iter_sema_type))
            {
                element_sema_type = rit->element;
                is_range = true;
                is_inclusive = true;
            }
            else if (auto const* st = types::type_cast<types::SliceType>(iter_sema_type))
            {
                element_sema_type = st->element;
                element_quals = st->element_quals;
            }
            else if (auto const* at = types::type_cast<types::ArrayType>(iter_sema_type))
                element_sema_type = at->element;
            else
                lower_panic(fs, std::format("for-in unsupported iterable type kind: {}", static_cast<int>(iter_sema_type->kind)));

            auto* ir_element_type = lower_type(element_sema_type);

            auto* usize_type = m_ctx.usize_t();

            if (is_range)
            {
                IrValue* start_val = nullptr;
                IrValue* end_val = nullptr;

                auto* range_expr = ast::node_cast<ast::RangeExpr>(iter_expr);
                if (!range_expr)
                    lower_panic(fs, "valued-range for-in must be sema-rejected; only literal RangeExpr allowed");

                if (range_expr->start)
                    start_val = lower_expr(range_expr->start);
                else
                    start_val = m_ctx.int_const(ir_element_type, 0);

                if (range_expr->end)
                    end_val = lower_expr(range_expr->end);
                else
                    lower_panic(fs, "for-in range must have an end bound");

                auto* counter_alloca = m_ctx.alloca(m_ctx.pointer_to(ir_element_type), ir_element_type);
                auto cnt_name = ident_name();
                counter_alloca->name = m_name_pool.back();
                append_inst(counter_alloca);
                append_inst(m_ctx.store(start_val, counter_alloca));

                auto* header_bb = create_block("forin.header");
                auto* body_bb = create_block("forin.body");
                auto* step_bb = create_block("forin.step");
                auto* exit_bb = create_block("forin.exit");

                emit_br(header_bb);

                bool is_signed_range = false;
                if (auto* int_ty = types::type_cast<types::IntType>(element_sema_type))
                    is_signed_range = int_ty->is_signed;

                set_current_block(header_bb);
                auto* cur_val = m_ctx.load(ir_element_type, counter_alloca);
                auto cur_name = ident_name();
                cur_val->name = m_name_pool.back();
                append_inst(cur_val);

                IrValue* cond = nullptr;
                if (is_inclusive)
                {
                    if (is_signed_range)
                        cond = m_ctx.cmp_le(cur_val, end_val);
                    else
                        cond = m_ctx.cmp_ule(cur_val, end_val);
                }
                else
                {
                    if (is_signed_range)
                        cond = m_ctx.cmp_lt(cur_val, end_val);
                    else
                        cond = m_ctx.cmp_ult(cur_val, end_val);
                }
                auto cond_name = ident_name();
                cond->name = m_name_pool.back();
                append_inst(cond);

                emit_br_cond(cond, body_bb, exit_bb);

                set_current_block(body_bb);
                push_loop(step_bb, exit_bb);

                {
                    push_scope();

                    auto* cur_val2 = m_ctx.load(ir_element_type, counter_alloca);
                    auto cur2_name = ident_name();
                    cur_val2->name = m_name_pool.back();
                    append_inst(cur_val2);

                    if (fs->item_type || !fs->item_name.empty())
                    {
                        if (fs->by_reference)
                        {
                            if (!fs->item_name.empty())
                            {
                                auto* ptr_sema_ty = make_ptr_sema_type(element_sema_type, element_quals);
                                m_named_values[fs->item_name] = MapEntry{counter_alloca, false, ptr_sema_ty, false};
                            }
                        }
                        else
                        {
                            auto* item_alloca = m_ctx.alloca(m_ctx.pointer_to(ir_element_type), ir_element_type);
                            auto item_name = ident_name();
                            item_alloca->name = m_name_pool.back();
                            append_inst(item_alloca);
                            append_inst(m_ctx.store(cur_val2, item_alloca));

                            if (!fs->item_name.empty())
                            {
                                auto* canon = get_canonical_type(fs->item_type && fs->item_type->sema.canonical ? fs->item_type : nullptr);
                                if (!canon)
                                    canon = element_sema_type;

                                m_named_values[fs->item_name] = MapEntry{item_alloca, true, canon, false};
                            }
                        }
                    }

                    for (auto* stmt : fs->body.stmts)
                        lower_stmt(stmt);

                    if (fs->body.tail && !current_block_terminated())
                        lower_expr(fs->body.tail);

                    pop_scope();
                }

                pop_loop();

                if (!current_block_terminated())
                    emit_br(step_bb);

                set_current_block(step_bb);

                auto* cur_val3 = m_ctx.load(ir_element_type, counter_alloca);
                auto cur3_name = ident_name();
                cur_val3->name = m_name_pool.back();
                append_inst(cur_val3);

                auto* one = m_ctx.int_const(ir_element_type, 1);
                auto* next_val = m_ctx.add(ir_element_type, cur_val3, one);
                auto next_name = ident_name();
                next_val->name = m_name_pool.back();
                append_inst(next_val);

                append_inst(m_ctx.store(next_val, counter_alloca));
                emit_br(header_bb);

                set_current_block(exit_bb);
                return;
            }

            auto* iter_val = lower_expr(iter_expr);

            IrValue* len_val = nullptr;
            IrValue* ptr_val = nullptr;

            if (iter_val->type && iter_val->type->kind == IrTypeKind::Slice)
            {
                auto* len_extract = m_ctx.extract(usize_type, iter_val, 1);
                auto len_name = ident_name();
                len_extract->name = m_name_pool.back();
                append_inst(len_extract);
                len_val = len_extract;

                auto* ptr_type = m_ctx.pointer_to(ir_element_type);
                auto* ptr_extract = m_ctx.extract(ptr_type, iter_val, 0);
                auto ptr_name = ident_name();
                ptr_extract->name = m_name_pool.back();
                append_inst(ptr_extract);
                ptr_val = ptr_extract;
            }
            else
            {
                auto* arr_type = static_cast<IrArrayType const*>(iter_val->type);
                if (!arr_type)
                    lower_panic(fs, "for-in iterable must be array or slice type at IR level");

                auto* arr_ptr_type = m_ctx.pointer_to(iter_val->type);
                auto* temp = m_ctx.alloca(arr_ptr_type, iter_val->type);
                auto temp_name = ident_name();
                temp->name = m_name_pool.back();
                append_inst(temp);
                append_inst(m_ctx.store(iter_val, temp));

                auto* gep0 = m_ctx.gep(m_ctx.pointer_to(ir_element_type), temp);
                gep0->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(usize_type, 0), 0});
                auto gep0_name = ident_name();
                gep0->name = m_name_pool.back();
                append_inst(gep0);
                ptr_val = gep0;

                len_val = m_ctx.int_const(usize_type, static_cast<std::int64_t>(arr_type->count));
            }

            auto* idx_alloca = m_ctx.alloca(m_ctx.pointer_to(usize_type), usize_type);
            auto idx_name = ident_name();
            idx_alloca->name = m_name_pool.back();
            append_inst(idx_alloca);
            append_inst(m_ctx.store(m_ctx.int_const(usize_type, 0), idx_alloca));

            auto* header_bb = create_block("forin.header");
            auto* body_bb = create_block("forin.body");
            auto* step_bb = create_block("forin.step");
            auto* exit_bb = create_block("forin.exit");

            emit_br(header_bb);

            set_current_block(header_bb);
            auto* cur_idx = m_ctx.load(usize_type, idx_alloca);
            auto idx_load_name = ident_name();
            cur_idx->name = m_name_pool.back();
            append_inst(cur_idx);

            auto* cond = m_ctx.cmp_ult(cur_idx, len_val);
            auto cond_name = ident_name();
            cond->name = m_name_pool.back();
            append_inst(cond);

            emit_br_cond(cond, body_bb, exit_bb);

            set_current_block(body_bb);
            push_loop(step_bb, exit_bb);

            {
                push_scope();

                auto* cur_idx2 = m_ctx.load(usize_type, idx_alloca);
                auto idx_load2_name = ident_name();
                cur_idx2->name = m_name_pool.back();
                append_inst(cur_idx2);

                if (m_bounds_check)
                {
                    auto forin_kind = (iter_val->type && iter_val->type->kind == IrTypeKind::Slice) ? BoundsCheckKind::Slice : BoundsCheckKind::Array;
                    emit_bounds_check(ptr_val, len_val, cur_idx2, iter_expr->range, {}, forin_kind);
                }

                auto* elem_ptr_type = m_ctx.pointer_to(ir_element_type);
                auto* gep = m_ctx.gep(elem_ptr_type, ptr_val);
                gep->indices.push_back({IrGepInst::IndexKind::Array, cur_idx2, 0});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);

                if (fs->item_type || !fs->item_name.empty())
                {
                    if (fs->by_reference)
                    {
                        if (!fs->item_name.empty())
                        {
                            auto* ptr_sema_ty = make_ptr_sema_type(element_sema_type, element_quals);
                            m_named_values[fs->item_name] = MapEntry{gep, false, ptr_sema_ty, false};
                        }
                    }
                    else
                    {
                        auto* item_alloca = m_ctx.alloca(m_ctx.pointer_to(ir_element_type), ir_element_type);
                        auto item_name = ident_name();
                        item_alloca->name = m_name_pool.back();
                        append_inst(item_alloca);

                        auto* elem_val = m_ctx.load(ir_element_type, gep);
                        auto load_name = ident_name();
                        elem_val->name = m_name_pool.back();
                        append_inst(elem_val);
                        append_inst(m_ctx.store(elem_val, item_alloca));

                        if (!fs->item_name.empty())
                        {
                            auto* canon = get_canonical_type(fs->item_type && fs->item_type->sema.canonical ? fs->item_type : nullptr);
                            if (!canon)
                                canon = element_sema_type;

                            m_named_values[fs->item_name] = MapEntry{item_alloca, true, canon, false};
                        }
                    }
                }

                for (auto* stmt : fs->body.stmts)
                    lower_stmt(stmt);

                if (fs->body.tail && !current_block_terminated())
                    lower_expr(fs->body.tail);

                pop_scope();
            }

            pop_loop();

            if (!current_block_terminated())
                emit_br(step_bb);

            set_current_block(step_bb);

            auto* cur_idx3 = m_ctx.load(usize_type, idx_alloca);
            auto idx_load3_name = ident_name();
            cur_idx3->name = m_name_pool.back();
            append_inst(cur_idx3);

            auto* one = m_ctx.int_const(usize_type, 1);
            auto* next_idx = m_ctx.add(usize_type, cur_idx3, one);
            auto next_name = ident_name();
            next_idx->name = m_name_pool.back();
            append_inst(next_idx);

            append_inst(m_ctx.store(next_idx, idx_alloca));
            emit_br(header_bb);

            set_current_block(exit_bb);
        }

        void lower_break_stmt(ast::BreakStmt const* bs)
        {
            std::ignore = bs;
            if (m_loop_stack.empty())
                lower_panic("break outside loop");

            auto& loop = m_loop_stack.back();

            flush_defers_to_depth(loop.scope_depth);
            emit_br(loop.exit_target);
        }

        void lower_continue_stmt(ast::ContinueStmt const* cs)
        {
            std::ignore = cs;
            if (m_loop_stack.empty())
                lower_panic("continue outside loop");

            auto& loop = m_loop_stack.back();

            flush_defers_to_depth(loop.scope_depth);
            emit_br(loop.continue_target);
        }

        enum class BoundsCheckKind : std::uint8_t
        {
            Slice,
            Array,
        };

        void emit_bounds_check(IrValue* ptr, IrValue* len, IrValue* index, sm::SourceRange range, sm::SourceRange fallback_range = {},
                               BoundsCheckKind kind = BoundsCheckKind::Array)
        {
            std::ignore = ptr;
            if (!m_bounds_check)
                return;

            auto* cmp_index = index;
            if (index->type && len->type && index->type->kind == IrTypeKind::Int && len->type->kind == IrTypeKind::Int)
            {
                auto const* idx_int = static_cast<IrIntType const*>(index->type);
                auto const* len_int = static_cast<IrIntType const*>(len->type);
                if (idx_int->bits != len_int->bits)
                {
                    if (idx_int->bits < len_int->bits)
                    {
                        auto* ext =
                            idx_int->is_signed ? static_cast<IrValue*>(m_ctx.sext(len->type, index)) : static_cast<IrValue*>(m_ctx.zext(len->type, index));
                        auto ext_name = ident_name();
                        ext->name = m_name_pool.back();
                        append_inst(ext);
                        cmp_index = ext;
                    }
                    else
                    {
                        auto* tr = m_ctx.trunc(len->type, index);
                        auto tr_name = ident_name();
                        tr->name = m_name_pool.back();
                        append_inst(tr);
                        cmp_index = tr;
                    }
                }
                else if (index->type != len->type)
                {
                    auto* bc = m_ctx.bitcast(len->type, cmp_index);
                    auto bc_name = ident_name();
                    bc->name = m_name_pool.back();
                    append_inst(bc);
                    cmp_index = bc;
                }
            }

            auto* cmp = m_ctx.cmp_ult(cmp_index, len);
            auto cmp_name = ident_name();
            cmp->name = m_name_pool.back();
            append_inst(cmp);

            auto* ok_bb = create_block("bounds.ok");
            auto* fail_bb = create_block("bounds.fail");

            emit_br_cond(cmp, ok_bb, fail_bb);

            set_current_block(fail_bb);

            std::string file = "<unknown>";
            int line = 0;
            std::string func = "<unknown>";

            if (m_current_func_decl && !m_current_func_decl->name.empty())
                func = std::string(m_current_func_decl->name);
            else if (m_current_func && !m_current_func->name.empty())
                func = std::string(m_current_func->name);

            sm::SourceRange resolved_range = range;
            if (!resolved_range.valid())
                resolved_range = fallback_range;
            if (!resolved_range.valid() && m_current_func_decl)
                resolved_range = m_current_func_decl->range;

            auto* sm_ptr = get_source_manager();
            if (sm_ptr && resolved_range.valid())
            {
                auto const* f = sm_ptr->get(resolved_range.begin.fileId);
                if (f)
                    file = f->path().filename().string();
                auto lc = sm_ptr->line_col(resolved_range.begin);
                if (lc)
                    line = static_cast<int>(lc->line);
            }

            std::string_view expr_str;
            switch (kind)
            {
                case BoundsCheckKind::Slice:
                    expr_str = "slice index out of bounds";
                    break;
                case BoundsCheckKind::Array:
                    expr_str = "array index out of bounds";
                    break;
            }

            emit_assert_call(file, line, func, expr_str);
            emit_unreachable();

            set_current_block(ok_bb);
        }

        void emit_assert_call(std::string_view file, int line, std::string_view func, std::string_view expr)
        {
            get_or_create_assert_func();

            auto* void_type = m_ctx.void_t();
            auto* call_inst = m_ctx.call(void_type, m_assert_func_ref);

            {
                auto* slice_ir_ty = m_ctx.slice_t(m_ctx.int_t(8, false), ir::Segment::None);
                auto* file_global = get_or_create_string_global(file, false);
                auto* file_ptr = m_ctx.global_ref(file_global, m_ctx.pointer_to(m_ctx.int_t(8, false)));
                auto* file_len = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(file.size()));
                auto* file_slice = m_ctx.aggregate(slice_ir_ty);
                file_slice->values.push_back(file_ptr);
                file_slice->values.push_back(file_len);
                call_inst->args.push_back(file_slice);
            }

            call_inst->args.push_back(m_ctx.int_const(m_ctx.int_t(32, true), line));

            {
                auto* slice_ir_ty = m_ctx.slice_t(m_ctx.int_t(8, false), ir::Segment::None);
                auto* func_global = get_or_create_string_global(func, false);
                auto* func_ptr = m_ctx.global_ref(func_global, m_ctx.pointer_to(m_ctx.int_t(8, false)));
                auto* func_len = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(func.size()));
                auto* func_slice = m_ctx.aggregate(slice_ir_ty);
                func_slice->values.push_back(func_ptr);
                func_slice->values.push_back(func_len);
                call_inst->args.push_back(func_slice);
            }

            {
                auto* slice_ir_ty = m_ctx.slice_t(m_ctx.int_t(8, false), ir::Segment::None);
                auto* expr_global = get_or_create_string_global(expr, false);
                auto* expr_ptr = m_ctx.global_ref(expr_global, m_ctx.pointer_to(m_ctx.int_t(8, false)));
                auto* expr_len = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(expr.size()));
                auto* expr_slice = m_ctx.aggregate(slice_ir_ty);
                expr_slice->values.push_back(expr_ptr);
                expr_slice->values.push_back(expr_len);
                call_inst->args.push_back(expr_slice);
            }

            append_inst(call_inst);
        }

        void get_or_create_assert_func()
        {
            if (m_assert_func)
                return;

            bool in_assert_module = false;
            if (m_module_path.size() == 1 && m_module_path[0] == "assert")
                in_assert_module = true;

            auto* void_type_ir = m_ctx.void_t();
            auto* slice_type_ir = m_ctx.slice_t(m_ctx.int_t(8, false), ir::Segment::None);
            auto* i32_type_ir = m_ctx.int_t(32, true);

            std::vector<IrType const*> params_ir = {slice_type_ir, i32_type_ir, slice_type_ir, slice_type_ir};
            auto* func_type = m_ctx.func_t(void_type_ir, params_ir);

            auto* assert_func_type = ir_type_cast<IrFuncType>(func_type);
            if (!assert_func_type)
                lower_panic("failed to create __assert function type");

            auto* void_sema = m_ctx.make<dcc::types::VoidType>();
            auto* char_sema = m_ctx.make<dcc::types::CharType>();
            auto* i32_sema = m_ctx.make<dcc::types::IntType>(static_cast<std::uint8_t>(32), true);
            auto* slice_const_char_sema = m_ctx.make<dcc::types::SliceType>(char_sema, dcc::types::Qual::Const, m_ctx.pointer_bits(), m_ctx.pointer_align());

            auto* synthetic_fd = m_ctx.make<ast::FuncDecl>(sm::SourceRange{}, std::string_view{"__assert"}, sm::SourceRange{});

            std::vector<dcc::types::TypePtr> param_sema_types = {slice_const_char_sema, i32_sema, slice_const_char_sema, slice_const_char_sema};
            std::array<std::string_view, 1> assert_module_path = {"assert"};

            m_name_pool.push_back(dcc::ir::mangle::mangle_function(std::span<std::string_view const>{assert_module_path}, *synthetic_fd, param_sema_types,
                                                                   void_sema, {}, m_nominal_resolver));

            std::string_view mangled_name = m_name_pool.back();

            if (in_assert_module)
            {
                IrFunction* self_func = nullptr;
                for (auto* f : m_module->functions)
                {
                    if (f->name == mangled_name)
                    {
                        self_func = f;
                        break;
                    }
                }
                if (!self_func)
                    lower_panic("__assert not found in function map while compiling assert module");
                m_assert_func = self_func;
                m_assert_func_ref = m_ctx.make<IrGlobalRef>(m_assert_func, func_type);
            }
            else
            {
                m_assert_func = m_ctx.function(mangled_name, assert_func_type);
                m_assert_func->is_dll_import = true;
                m_assert_func->linkage = ir::Linkage::External;
                m_module->functions.push_back(m_assert_func);

                m_assert_func_ref = m_ctx.make<IrGlobalRef>(m_assert_func, func_type);
            }
        }

        dcc::sm::SourceManager const* get_source_manager() const { return m_source_manager; }

        void lower_stmt(ast::Stmt const* stmt)
        {
            if (!stmt)
                return;

            SourceRangeGuard guard(*this, stmt->range);

            switch (stmt->kind)
            {
                case ast::StmtKind::Return: {
                    lower_return_stmt(static_cast<ast::ReturnStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::Expr: {
                    auto* es = static_cast<ast::ExprStmt const*>(stmt);
                    if (es->expr)
                        lower_expr(es->expr);
                    break;
                }

                case ast::StmtKind::DeclStmt: {
                    auto* ds = static_cast<ast::DeclStmt const*>(stmt);
                    auto* vd = ast::node_cast<ast::VarDecl>(ds->decl);
                    if (!vd)
                        lower_unimplemented(ds, "DeclStmt non-VarDecl");

                    auto* canon = get_canonical_type(vd->type);
                    if (!canon)
                        lower_panic(ds, "VarDecl without canonical type");

                    if (auto* rta = types::type_cast<types::RuntimeArrayType>(canon))
                    {
                        auto* ir_elem_type = lower_type(rta->element);
                        auto* ptr_type = m_ctx.pointer_to(ir_elem_type, ir::Segment::None);

                        IrValue* count_val = nullptr;
                        if (vd->type && vd->type->kind == ast::TypeKind::Array)
                        {
                            auto const* arr_type = static_cast<ast::ArrayType const*>(vd->type);
                            if (arr_type->size)
                            {
                                if (arr_type->size->kind == ast::ExprKind::Ident)
                                {
                                    auto const* id = static_cast<ast::IdentExpr const*>(arr_type->size);
                                    auto name_it = m_named_values.find(id->name);
                                    if (name_it != m_named_values.end())
                                    {
                                        if (name_it->second.is_storage)
                                        {
                                            auto* ir_ty = lower_type(name_it->second.sema_type);
                                            count_val = m_ctx.load(ir_ty, name_it->second.value);
                                            auto load_name = ident_name();
                                            count_val->name = m_name_pool.back();
                                            append_inst(count_val);
                                        }
                                        else
                                            count_val = name_it->second.value;
                                    }
                                    if (!count_val && arr_type->size->sema.resolved_decl)
                                    {
                                        auto it = m_value_map.find(arr_type->size->sema.resolved_decl);
                                        if (it != m_value_map.end())
                                        {
                                            auto& entry = it->second;
                                            if (entry.is_storage)
                                            {
                                                auto* ir_ty = lower_type(entry.sema_type);
                                                count_val = m_ctx.load(ir_ty, entry.value);
                                                auto load_name = ident_name();
                                                count_val->name = m_name_pool.back();
                                                append_inst(count_val);
                                            }
                                            else
                                                count_val = entry.value;
                                        }
                                    }
                                }
                                if (!count_val)
                                    count_val = lower_expr(arr_type->size);
                            }
                        }

                        if (!count_val)
                            lower_panic(ds, "RuntimeArray without count value");

                        auto* alloca = m_ctx.alloca(ptr_type, ir_elem_type, count_val);
                        alloca->alignment = vd->sema.alignment;

                        auto name = ident_name();
                        alloca->name = m_name_pool.back();
                        append_inst(alloca);

                        bool is_volatile = false;
                        if (vd->type)
                        {
                            if (auto* qt = ast::node_cast<ast::QualifiedType>(vd->type))
                                is_volatile = ast::has_qual(qt->quals, ast::Qual::Volatile);
                        }

                        m_value_map[vd] = MapEntry{alloca, true, canon, is_volatile};
                        break;
                    }

                    auto* ir_alloc_type = lower_type(canon);
                    auto* ptr_type = m_ctx.pointer_to(ir_alloc_type, ir::Segment::None);
                    auto* alloca = m_ctx.alloca(ptr_type, ir_alloc_type);
                    alloca->alignment = vd->sema.alignment;

                    auto name = ident_name();
                    alloca->name = m_name_pool.back();
                    append_inst(alloca);

                    bool is_volatile = false;
                    if (vd->type)
                    {
                        if (auto* qt = ast::node_cast<ast::QualifiedType>(vd->type))
                            is_volatile = ast::has_qual(qt->quals, ast::Qual::Volatile);
                    }

                    m_value_map[vd] = MapEntry{alloca, true, canon, is_volatile};

                    if (vd->init)
                    {
                        auto* val = lower_implicit_enum_construction(vd->init, [&]() { return lower_expr(vd->init); });
                        auto* init_sema_type = get_sema_resolved_type(vd->init);
                        if (canon && canon->kind == types::TypeKind::Slice && init_sema_type && init_sema_type->kind == types::TypeKind::Array)
                            val = coerce_array_to_slice(val, init_sema_type, canon);

                        if (is_volatile)
                            append_inst(m_ctx.store_volatile(val, alloca));
                        else
                            append_inst(m_ctx.store(val, alloca));
                    }
                    break;
                }

                case ast::StmtKind::While: {
                    lower_while_stmt(static_cast<ast::WhileStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::DoWhile: {
                    lower_dowhile_stmt(static_cast<ast::DoWhileStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::For: {
                    lower_for_stmt(static_cast<ast::ForStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::ForIn: {
                    lower_forin_stmt(static_cast<ast::ForInStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::Break: {
                    lower_break_stmt(static_cast<ast::BreakStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::Continue: {
                    lower_continue_stmt(static_cast<ast::ContinueStmt const*>(stmt));
                    break;
                }

                case ast::StmtKind::Defer: {
                    auto* ds = static_cast<ast::DeferStmt const*>(stmt);
                    register_defer(ds->body);
                    break;
                }

                case ast::StmtKind::StaticFor: {
                    lower_panic(stmt, "StaticFor reached IR lowering");
                }

                case ast::StmtKind::Ambiguous: {
                    lower_panic(stmt, "unresolved ambiguous statement reached IR lowering");
                }

                default: {
                    std::string reason = std::format("unsupported statement kind: {}", static_cast<int>(stmt->kind));
                    lower_unimplemented(stmt, reason);
                }
            }
        }

        IrValue* lower_expr(ast::Expr const* expr)
        {
            if (!expr)
                lower_panic("null expression");

            SourceRangeGuard guard(*this, expr->range);

            if (expr->kind == ast::ExprKind::Ident)
                return lower_ident_expr(static_cast<ast::IdentExpr const*>(expr));

            if (expr->kind == ast::ExprKind::Call)
                return lower_call_expr(static_cast<ast::CallExpr const*>(expr));

            if (expr->sema.const_value)
            {
                bool pure = (expr->kind == ast::ExprKind::IntLiteral || expr->kind == ast::ExprKind::FloatLiteral || expr->kind == ast::ExprKind::BoolLiteral ||
                             expr->kind == ast::ExprKind::CharLiteral || expr->kind == ast::ExprKind::U16CharLiteral ||
                             expr->kind == ast::ExprKind::NullLiteral || expr->kind == ast::ExprKind::Cast || expr->kind == ast::ExprKind::Sizeof ||
                             expr->kind == ast::ExprKind::SizeofPack || expr->kind == ast::ExprKind::Alignof || expr->kind == ast::ExprKind::Offsetof);

                if (pure)
                    return materialize_comptime(*expr->sema.const_value, get_sema_resolved_type(expr));
            }

            switch (expr->kind)
            {
                case ast::ExprKind::IntLiteral: {
                    auto* lit = static_cast<ast::IntLiteralExpr const*>(expr);
                    auto* ty = get_sema_resolved_type(expr);
                    return m_ctx.int_const(lower_type(ty), lit->value);
                }

                case ast::ExprKind::FloatLiteral: {
                    auto* lit = static_cast<ast::FloatLiteralExpr const*>(expr);
                    auto* ty = get_sema_resolved_type(expr);
                    return m_ctx.float_const(lower_type(ty), lit->value);
                }

                case ast::ExprKind::BoolLiteral: {
                    auto* lit = static_cast<ast::BoolLiteralExpr const*>(expr);
                    return m_ctx.bool_const(lit->value);
                }

                case ast::ExprKind::CharLiteral: {
                    auto* lit = static_cast<ast::CharLiteralExpr const*>(expr);
                    return m_ctx.int_const(m_ctx.int_t(8, false), static_cast<std::int64_t>(lit->codepoint));
                }

                case ast::ExprKind::U16CharLiteral: {
                    auto* lit = static_cast<ast::U16CharLiteralExpr const*>(expr);
                    return m_ctx.int_const(m_ctx.int_t(16, false), static_cast<std::int64_t>(lit->value));
                }

                case ast::ExprKind::NullLiteral: {
                    auto* ty = get_sema_resolved_type(expr);
                    return m_ctx.null_const(lower_type(ty));
                }

                case ast::ExprKind::Unary: {
                    return lower_unary_expr(static_cast<ast::UnaryExpr const*>(expr));
                }

                case ast::ExprKind::Postfix: {
                    return lower_postfix_expr(static_cast<ast::PostfixExpr const*>(expr));
                }

                case ast::ExprKind::Binary: {
                    return lower_binary_expr(static_cast<ast::BinaryExpr const*>(expr));
                }

                case ast::ExprKind::Cast: {
                    return lower_cast_expr(static_cast<ast::CastExpr const*>(expr));
                }

                case ast::ExprKind::If: {
                    return lower_if_expr(static_cast<ast::IfExpr const*>(expr));
                }

                case ast::ExprKind::Match: {
                    return lower_match_expr(static_cast<ast::MatchExpr const*>(expr));
                }

                case ast::ExprKind::Block: {
                    return lower_block_expr(static_cast<ast::BlockExpr const*>(expr));
                }

                case ast::ExprKind::Call: {
                    return lower_call_expr(static_cast<ast::CallExpr const*>(expr));
                }

                case ast::ExprKind::StringLiteral: {
                    auto* sl = static_cast<ast::StringLiteralExpr const*>(expr);
                    if (expr->sema.construction_kind == ast::ExprSema::ConstructionKind::Enum && expr->sema.constructed_variant)
                    {
                        auto* variant = expr->sema.constructed_variant;
                        auto* enum_type = get_sema_resolved_type(expr);
                        auto* et = types::type_cast<types::EnumType>(enum_type);
                        if (et && et->is_tagged && !variant->payload.empty())
                        {
                            auto* payload_canon = get_canonical_type(variant->payload[0]);
                            if (payload_canon)
                            {
                                auto* payload_val = lower_string_literal_value(sl, payload_canon);
                                std::vector<IrValue*> payload_args{payload_val};
                                return lower_tagged_enum_construction(variant, enum_type, payload_args);
                            }
                        }
                    }
                    auto* target_type = get_sema_resolved_type(expr);
                    return lower_string_literal_value(sl, target_type);
                }

                case ast::ExprKind::U16StringLiteral: {
                    auto* sl = static_cast<ast::U16StringLiteralExpr const*>(expr);
                    if (expr->sema.construction_kind == ast::ExprSema::ConstructionKind::Enum && expr->sema.constructed_variant)
                    {
                        auto* variant = expr->sema.constructed_variant;
                        auto* enum_type = get_sema_resolved_type(expr);
                        auto* et = types::type_cast<types::EnumType>(enum_type);
                        if (et && et->is_tagged && !variant->payload.empty())
                        {
                            auto* payload_canon = get_canonical_type(variant->payload[0]);
                            if (payload_canon)
                            {
                                auto* payload_val = lower_u16_string_literal_value(sl, payload_canon);
                                std::vector<IrValue*> payload_args{payload_val};
                                return lower_tagged_enum_construction(variant, enum_type, payload_args);
                            }
                        }
                    }
                    auto* target_type = get_sema_resolved_type(expr);
                    return lower_u16_string_literal_value(sl, target_type);
                }

                case ast::ExprKind::FieldAccess: {
                    return lower_field_access_expr(static_cast<ast::FieldAccessExpr const*>(expr));
                }

                case ast::ExprKind::Index: {
                    return lower_index_expr(static_cast<ast::IndexExpr const*>(expr));
                }

                case ast::ExprKind::PackAccess: {
                    lower_panic(expr, "PackAccess must be resolved before IR lowering");
                }

                case ast::ExprKind::StructLiteral: {
                    return lower_struct_literal_expr(static_cast<ast::StructLiteralExpr const*>(expr));
                }

                case ast::ExprKind::PathExpr: {
                    return lower_path_expr(static_cast<ast::PathExpr const*>(expr));
                }

                case ast::ExprKind::SizeofPack: {
                    lower_panic(expr, "SizeofPack reached switch without a const_value");
                }

                case ast::ExprKind::PackExpansion: {
                    lower_panic(expr, "PackExpansion reached IR lowering");
                }

                case ast::ExprKind::Alignof: {
                    lower_panic(expr, "Alignof reached switch without a const_value");
                }

                case ast::ExprKind::Offsetof: {
                    lower_panic(expr, "Offsetof reached switch without a const_value");
                }

                case ast::ExprKind::Range: {
                    lower_panic(expr, "RangeExpr must not reach IR expression lowering as runtime value");
                }

                default: {
                    std::string reason = std::format("unsupported expression kind: {}", static_cast<int>(expr->kind));
                    lower_unimplemented(expr, reason);
                }
            }
        }

        IrValue* lower_call_expr(ast::CallExpr const* call)
        {
            if (call->sema.construction_kind == ast::ExprSema::ConstructionKind::Enum && call->sema.constructed_variant)
            {
                auto* enum_type = get_sema_resolved_type(call);
                auto* et = types::type_cast<types::EnumType>(enum_type);
                if (et && et->is_tagged)
                {
                    std::vector<IrValue*> payload_args;
                    for (std::size_t i = 0; i < call->args.size(); ++i)
                        payload_args.push_back(lower_expr(call->args[i]));

                    return lower_tagged_enum_construction(call->sema.constructed_variant, enum_type, payload_args);
                }
            }

            IrValue* callee_value = nullptr;

            auto* resolved_spec = call->sema.resolved_specialization;
            auto* callee_expr = call->callee;

            ast::FuncDecl const* direct_target = resolved_spec;

            if (!direct_target)
            {
                if (auto* ident = ast::node_cast<ast::IdentExpr>(callee_expr))
                {
                    if (ident->sema.resolved_decl)
                    {
                        auto* fd = ast::node_cast<ast::FuncDecl>(ident->sema.resolved_decl);
                        if (fd)
                            direct_target = fd;
                    }
                }
                else if (auto* path_expr = ast::node_cast<ast::PathExpr>(callee_expr))
                {
                    if (path_expr->sema.resolved_decl)
                    {
                        auto* fd = ast::node_cast<ast::FuncDecl>(path_expr->sema.resolved_decl);
                        if (fd)
                            direct_target = fd;
                    }
                }

                if (!direct_target && call->sema.ufcs_callee)
                {
                    auto* fd = ast::node_cast<ast::FuncDecl>(call->sema.ufcs_callee);
                    if (fd)
                        direct_target = fd;
                }

                if (!direct_target && call->sema.resolved_decl)
                {
                    auto* fd = ast::node_cast<ast::FuncDecl>(call->sema.resolved_decl);
                    if (fd)
                        direct_target = fd;
                }
            }

            if (direct_target && direct_target->sema.is_intrinsic)
                return lower_intrinsic_call(direct_target, call);

            if (direct_target)
            {
                auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(direct_target));
                if (!ir_func)
                    lower_panic(call, std::format("call target function `{}` not in function map", direct_target->name));
                callee_value = m_ctx.func_ref(ir_func);
            }
            else
                callee_value = lower_expr(callee_expr);

            auto* sema_ret_type = get_sema_resolved_type(call);
            auto* ir_ret_type = lower_type(sema_ret_type);

            auto* call_inst = m_ctx.call(ir_ret_type, callee_value);

            if (call->sema.ufcs_callee)
            {
                auto* field_access = ast::node_cast<ast::FieldAccessExpr>(callee_expr);
                if (field_access)
                {
                    auto* obj_val = lower_expr(field_access->object);
                    call_inst->args.push_back(obj_val);
                }
            }

            auto* callee_decl = direct_target;
            for (std::size_t i = 0; i < call->args.size(); ++i)
            {
                auto* arg_expr = call->args[i];

                auto* arg_val = lower_implicit_enum_construction(arg_expr, [&]() { return lower_expr(arg_expr); });

                auto* arg_sema_type = get_sema_resolved_type(arg_expr);

                if (callee_decl && i < callee_decl->params.size())
                {
                    auto* param_type = get_canonical_type(callee_decl->params[i].type);
                    if (param_type && param_type->kind == types::TypeKind::Slice && arg_sema_type && arg_sema_type->kind == types::TypeKind::Array)
                        arg_val = coerce_array_to_slice(arg_val, arg_sema_type, param_type);
                }

                call_inst->args.push_back(arg_val);
            }

            bool is_void_result = (ir_ret_type->kind == IrTypeKind::Void);
            if (!is_void_result)
            {
                auto name = ident_name();
                call_inst->name = m_name_pool.back();
            }

            append_inst(call_inst);

            return is_void_result ? nullptr : call_inst;
        }

        [[nodiscard]] IrMemoryOrdering extract_order_from_arg(IrValue* order_val, ast::CallExpr const* call) const
        {
            auto* order_int = ir_cast<IrIntConstant>(order_val);
            if (!order_int)
                lower_panic(call, "MemoryOrder argument must be a compile-time constant");

            auto raw = order_int->value;
            if (raw < 0 || raw > 4)
                lower_panic(call, std::format("invalid MemoryOrder value: {}", raw));

            return static_cast<IrMemoryOrdering>(raw);
        }

        [[nodiscard]] IrValue* maybe_unwrap_atomic_ptr(IrValue* ptr)
        {
            if (!ptr || !ptr->type)
                return ptr;

            auto* ptr_type = ir_type_cast<IrPointerType>(ptr->type);
            if (!ptr_type)
                return ptr;

            auto* pointee = ptr_type->pointee;
            if (!pointee || pointee->kind != IrTypeKind::Aggregate)
                return ptr;

            auto* agg_type = static_cast<IrAggregateType const*>(pointee);
            if (agg_type->members.size() != 1)
                return ptr;

            auto* field_type = agg_type->members[0];
            auto* field_ptr_type = m_ctx.pointer_to(field_type, ptr_type->seg);
            auto* gep = m_ctx.gep(field_ptr_type, ptr);
            gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, 0});
            auto name = ident_name();
            gep->name = m_name_pool.back();
            append_inst(gep);
            return gep;
        }

        IrValue* lower_intrinsic_call(ast::FuncDecl const* func, ast::CallExpr const* call)
        {
            auto name = func->name;

            std::vector<IrValue*> args;
            for (auto* arg_expr : call->args)
                args.push_back(lower_expr(arg_expr));

            auto* sema_ret = get_sema_resolved_type(call);
            auto* ir_ret = lower_type(sema_ret);

            IrValue* result = nullptr;

            auto get_atomic_ptr = [&](std::size_t idx) -> IrValue* {
                if (idx >= args.size())
                    lower_panic(call, std::format("missing pointer argument at index {}", idx));
                return maybe_unwrap_atomic_ptr(args[idx]);
            };

            if (name == "atomic_load")
            {
                if (args.size() < 2)
                    lower_panic(call, "atomic_load requires 2 arguments (ptr, order)");
                auto order = extract_order_from_arg(args[1], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_load(ir_ret, ptr, order);
            }
            else if (name == "atomic_store")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_store requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_store(args[1], ptr, order);
            }
            else if (name == "atomic_exchange")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_exchange requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_rmw(ir_ret, IrAtomicRmwOp::Xchg, ptr, args[1], order);
            }
            else if (name == "atomic_fetch_add")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_fetch_add requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_rmw(ir_ret, IrAtomicRmwOp::Add, ptr, args[1], order);
            }
            else if (name == "atomic_fetch_sub")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_fetch_sub requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_rmw(ir_ret, IrAtomicRmwOp::Sub, ptr, args[1], order);
            }
            else if (name == "atomic_fetch_and")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_fetch_and requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_rmw(ir_ret, IrAtomicRmwOp::And, ptr, args[1], order);
            }
            else if (name == "atomic_fetch_or")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_fetch_or requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_rmw(ir_ret, IrAtomicRmwOp::Or, ptr, args[1], order);
            }
            else if (name == "atomic_fetch_xor")
            {
                if (args.size() < 3)
                    lower_panic(call, "atomic_fetch_xor requires 3 arguments (ptr, value, order)");
                auto order = extract_order_from_arg(args[2], call);
                auto* ptr = get_atomic_ptr(0);
                result = m_ctx.atomic_rmw(ir_ret, IrAtomicRmwOp::Xor, ptr, args[1], order);
            }
            else if (name == "atomic_fence")
            {
                if (args.size() < 1)
                    lower_panic(call, "atomic_fence requires 1 argument (order)");
                auto order = extract_order_from_arg(args[0], call);
                result = m_ctx.fence(order);
            }
            else
                lower_panic(call, std::format("unknown intrinsic function `{}`", name));

            if (result && result->kind != IrNodeKind::Fence && result->type && result->type->kind != IrTypeKind::Void)
            {
                auto name_str = ident_name();
                result->name = m_name_pool.back();
            }

            append_inst(result);
            return (result && result->type && result->type->kind != IrTypeKind::Void) ? result : nullptr;
        }

        IrValue* lower_ident_expr(ast::IdentExpr const* id)
        {
            auto* resolved = id->sema.resolved_decl;

            if (resolved)
            {
                {
                    auto it = m_value_map.find(resolved);
                    if (it != m_value_map.end())
                    {
                        auto& entry = it->second;
                        if (entry.is_storage)
                        {
                            auto* ir_type = lower_type(entry.sema_type);
                            IrValue* loaded;
                            if (entry.is_volatile)
                                loaded = m_ctx.load_volatile(ir_type, entry.value);
                            else
                                loaded = m_ctx.load(ir_type, entry.value);

                            auto name = ident_name();
                            loaded->name = m_name_pool.back();
                            append_inst(loaded);
                            return loaded;
                        }

                        return entry.value;
                    }
                }

                {
                    auto name_it = m_named_values.find(id->name);
                    if (name_it != m_named_values.end())
                    {
                        auto& entry = name_it->second;
                        if (entry.is_storage)
                        {
                            auto* ir_type = lower_type(entry.sema_type);
                            auto* loaded = m_ctx.load(ir_type, entry.value);
                            auto name = ident_name();
                            loaded->name = m_name_pool.back();
                            append_inst(loaded);
                            return loaded;
                        }
                        return entry.value;
                    }
                }

                if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                {
                    auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                    if (global)
                    {
                        auto* ptr_type = m_ctx.pointer_to(global->type);
                        auto* global_ref = m_ctx.global_ref(global, ptr_type);

                        auto* loaded = m_ctx.load(global->type, global_ref);
                        auto name = ident_name();
                        loaded->name = m_name_pool.back();
                        append_inst(loaded);
                        return loaded;
                    }
                }

                if (auto* fd = ast::node_cast<ast::FuncDecl>(resolved))
                {
                    auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(fd));
                    if (ir_func)
                        return m_ctx.func_ref(ir_func);

                    lower_panic(id, std::format("function `{}` referenced but not in function map", id->name));
                }
            }

            if (id->sema.const_value)
                return materialize_comptime(*id->sema.const_value, get_sema_resolved_type(id));

            if (!resolved)
                lower_panic(id, std::format("IdentExpr `{}` missing resolved_decl", id->name));

            lower_panic(id, std::format("identifier `{}` not in value map", id->name));
        }

        IrValue* lower_path_expr(ast::PathExpr const* pe)
        {
            if (pe->sema.construction_kind == ast::ExprSema::ConstructionKind::Enum && pe->sema.constructed_variant)
            {
                auto* enum_type = get_sema_resolved_type(pe);
                auto* et = types::type_cast<types::EnumType>(enum_type);
                if (et && et->is_tagged)
                    return lower_tagged_enum_construction(pe->sema.constructed_variant, enum_type, std::span<IrValue*>{});

                auto* ir_ty = lower_type(enum_type);
                return m_ctx.int_const(ir_ty, static_cast<std::int64_t>(pe->sema.constructed_variant->discriminant));
            }

            auto* resolved = pe->sema.resolved_decl;
            if (!resolved)
                lower_panic(pe, "PathExpr missing resolved_decl");

            if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
            {
                auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                if (global)
                {
                    auto* ptr_type = m_ctx.pointer_to(global->type);
                    auto* global_ref = m_ctx.global_ref(global, ptr_type);
                    auto* loaded = m_ctx.load(global->type, global_ref);
                    auto name = ident_name();
                    loaded->name = m_name_pool.back();
                    append_inst(loaded);
                    return loaded;
                }
            }

            if (auto* fd = ast::node_cast<ast::FuncDecl>(resolved))
            {
                auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(fd));
                if (ir_func)
                    return m_ctx.func_ref(ir_func);
            }

            lower_panic(pe, "PathExpr cannot be lowered");
        }

        IrValue* lower_unary_expr(ast::UnaryExpr const* u)
        {
            auto* ty = get_sema_resolved_type(u);
            auto* ir_ty = lower_type(ty);

            if (u->op == dcc::lex::TokenKind::Increment)
                return lower_pre_incdec(u->operand, ir_ty, true);
            if (u->op == dcc::lex::TokenKind::Decrement)
                return lower_pre_incdec(u->operand, ir_ty, false);
            if (u->op == dcc::lex::TokenKind::Amp)
                return lower_addr_of(u->operand);

            auto* operand = lower_expr(u->operand);

            switch (u->op)
            {
                case dcc::lex::TokenKind::Plus:
                    return operand;

                case dcc::lex::TokenKind::Minus: {
                    auto* inst = m_ctx.neg(ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);
                    return inst;
                }

                case dcc::lex::TokenKind::Bang: {
                    auto* zero = m_ctx.bool_const(false);
                    auto* inst = m_ctx.cmp_eq(operand, zero);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);
                    return inst;
                }

                case dcc::lex::TokenKind::Tilde: {
                    auto* inst = m_ctx.not_(ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);
                    return inst;
                }

                case dcc::lex::TokenKind::Star: {
                    auto* loaded = m_ctx.load(ir_ty, operand);
                    auto name = ident_name();
                    loaded->name = m_name_pool.back();
                    append_inst(loaded);
                    return loaded;
                }

                case dcc::lex::TokenKind::Amp: {
                    return lower_addr_of(u->operand);
                }

                default:
                    lower_unimplemented(u, std::format("unsupported unary op: {}", static_cast<int>(u->op)));
            }
        }

        IrValue* lower_addr_of(ast::Expr const* operand)
        {
            if (operand->kind == ast::ExprKind::Ident)
            {
                auto* id = static_cast<ast::IdentExpr const*>(operand);
                auto* resolved = id->sema.resolved_decl;
                if (resolved)
                {
                    if (auto* fd = ast::node_cast<ast::FuncDecl>(resolved))
                    {
                        auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(fd));
                        if (ir_func)
                            return m_ctx.func_ref(ir_func);
                        lower_panic(id, std::format("function `{}` not in function map for address-of", id->name));
                    }

                    {
                        auto it = m_value_map.find(resolved);
                        if (it != m_value_map.end())
                        {
                            if (!it->second.is_storage)
                            {
                                auto* sema_ty = get_sema_resolved_type(operand);
                                auto* ir_ty = lower_type(sema_ty);
                                auto* ptr_type = m_ctx.pointer_to(ir_ty, ir::Segment::None);
                                auto* alloca = m_ctx.alloca(ptr_type, ir_ty);
                                auto alloca_name = ident_name();
                                alloca->name = m_name_pool.back();
                                append_inst(alloca);
                                append_inst(m_ctx.store(it->second.value, alloca));
                                return alloca;
                            }

                            return it->second.value;
                        }
                    }

                    if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                    {
                        auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                        if (global)
                        {
                            auto* ptr_type = m_ctx.pointer_to(global->type);
                            return m_ctx.global_ref(global, ptr_type);
                        }
                    }

                    lower_panic(id, std::format("identifier `{}` not found for address-of", id->name));
                }
            }

            if (operand->kind == ast::ExprKind::TemplateInst)
            {
                auto* res_spec = operand->sema.resolved_specialization;
                if (res_spec)
                {
                    auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(res_spec));
                    if (ir_func)
                        return m_ctx.func_ref(ir_func);
                    lower_panic(std::format("specialization `{}` not in function map for address-of", res_spec->name));
                }
                lower_panic("TemplateInst has no resolved specialization for address-of");
            }

            if (operand->kind == ast::ExprKind::FieldAccess)
                return lower_field_lvalue(static_cast<ast::FieldAccessExpr const*>(operand));

            if (operand->kind == ast::ExprKind::Index)
                return lower_index_lvalue(static_cast<ast::IndexExpr const*>(operand));

            if (operand->kind == ast::ExprKind::PackAccess)
                lower_panic(operand, "PackAccess must be resolved before IR lowering");

            {
                auto* sema_ty = get_sema_resolved_type(operand);
                if (sema_ty)
                {
                    auto* val = lower_expr(operand);
                    auto* ir_ty = lower_type(sema_ty);
                    auto* ptr_type = m_ctx.pointer_to(ir_ty, ir::Segment::None);
                    auto* alloca = m_ctx.alloca(ptr_type, ir_ty);
                    auto alloca_name = ident_name();
                    alloca->name = m_name_pool.back();
                    append_inst(alloca);
                    append_inst(m_ctx.store(val, alloca));
                    return alloca;
                }
            }

            lower_unimplemented(operand, "address-of unsupported");
        }

        IrValue* lower_pre_incdec(ast::Expr const* operand, IrType const* ir_ty, bool is_inc)
        {
            auto lv = lower_assign_lvalue(operand);
            if (!lv.entry && !lv.gep_ptr)
                lower_unimplemented(operand, "pre-inc/dec on unsupported lvalue");

            IrValue* ptr;
            bool is_volatile = false;
            if (lv.entry)
            {
                if (!lv.entry->is_storage)
                    lower_panic("inc/dec requires storage");
                ptr = lv.entry->value;
                is_volatile = lv.entry->is_volatile;
            }
            else
                ptr = lv.gep_ptr;

            auto* loaded = m_ctx.load(ir_ty, ptr);
            auto load_name = ident_name();
            loaded->name = m_name_pool.back();
            append_inst(loaded);

            auto* one = m_ctx.int_const(ir_ty, 1);
            IrValue* result;
            if (is_inc)
                result = m_ctx.add(ir_ty, loaded, one);
            else
                result = m_ctx.sub(ir_ty, loaded, one);
            auto result_name = ident_name();
            result->name = m_name_pool.back();
            append_inst(result);

            if (is_volatile)
                append_inst(m_ctx.store_volatile(result, ptr));
            else
                append_inst(m_ctx.store(result, ptr));

            return result;
        }

        IrValue* lower_postfix_expr(ast::PostfixExpr const* p)
        {
            auto* ty = get_sema_resolved_type(p);
            auto* ir_ty = lower_type(ty);

            switch (p->op)
            {
                case dcc::lex::TokenKind::Increment:
                    return lower_post_incdec(p->operand, ir_ty, true);

                case dcc::lex::TokenKind::Decrement:
                    return lower_post_incdec(p->operand, ir_ty, false);

                default:
                    lower_unimplemented(p, std::format("unsupported postfix op: {}", static_cast<int>(p->op)));
            }
        }

        IrValue* lower_post_incdec(ast::Expr const* operand, IrType const* ir_ty, bool is_inc)
        {
            auto lv = lower_assign_lvalue(operand);
            if (!lv.entry && !lv.gep_ptr)
                lower_unimplemented(operand, "post-inc/dec on unsupported lvalue");

            IrValue* ptr;
            bool is_volatile = false;
            if (lv.entry)
            {
                if (!lv.entry->is_storage)
                    lower_panic("post-inc/dec requires storage");
                ptr = lv.entry->value;
                is_volatile = lv.entry->is_volatile;
            }
            else
                ptr = lv.gep_ptr;

            auto* loaded = m_ctx.load(ir_ty, ptr);
            auto load_name = ident_name();
            loaded->name = m_name_pool.back();
            append_inst(loaded);

            auto* one = m_ctx.int_const(ir_ty, 1);
            IrValue* updated;
            if (is_inc)
                updated = m_ctx.add(ir_ty, loaded, one);
            else
                updated = m_ctx.sub(ir_ty, loaded, one);

            auto result_name = ident_name();
            updated->name = m_name_pool.back();
            append_inst(updated);

            if (is_volatile)
                append_inst(m_ctx.store_volatile(updated, ptr));
            else
                append_inst(m_ctx.store(updated, ptr));

            return loaded;
        }

        IrValue* lower_binary_expr(ast::BinaryExpr const* bin)
        {
            auto* lhs = bin->lhs;
            auto* rhs = bin->rhs;
            auto* sema_ty = get_sema_resolved_type(bin);
            auto* ir_ty = lower_type(sema_ty);

            if (bin->op == dcc::lex::TokenKind::AmpAmp)
                return lower_short_circuit_and(bin, sema_ty, ir_ty);
            if (bin->op == dcc::lex::TokenKind::PipePipe)
                return lower_short_circuit_or(bin, sema_ty, ir_ty);

            if (bin->op == dcc::lex::TokenKind::Eq)
                return lower_assign(bin, sema_ty, ir_ty);

            if (is_compound_assign(bin->op))
                return lower_compound_assign(bin, sema_ty, ir_ty);

            auto* lhs_val = lower_expr(lhs);
            auto* rhs_val = lower_expr(rhs);

            auto* op_sema_ty = get_sema_resolved_type(bin->lhs);
            auto* inst = make_binop(ir_ty, lhs_val, rhs_val, bin->op, op_sema_ty);
            auto name = ident_name();
            inst->name = m_name_pool.back();
            append_inst(inst);

            return inst;
        }

        static bool is_compound_assign(dcc::lex::TokenKind op)
        {
            return op == dcc::lex::TokenKind::PlusEq || op == dcc::lex::TokenKind::MinusEq || op == dcc::lex::TokenKind::StarEq ||
                   op == dcc::lex::TokenKind::SlashEq || op == dcc::lex::TokenKind::PercentEq || op == dcc::lex::TokenKind::AmpEq ||
                   op == dcc::lex::TokenKind::PipeEq || op == dcc::lex::TokenKind::CaretEq || op == dcc::lex::TokenKind::LtLtEq ||
                   op == dcc::lex::TokenKind::GtGtEq;
        }

        static dcc::lex::TokenKind compound_to_binop(dcc::lex::TokenKind op)
        {
            switch (op)
            {
                case dcc::lex::TokenKind::PlusEq:
                    return dcc::lex::TokenKind::Plus;
                case dcc::lex::TokenKind::MinusEq:
                    return dcc::lex::TokenKind::Minus;
                case dcc::lex::TokenKind::StarEq:
                    return dcc::lex::TokenKind::Star;
                case dcc::lex::TokenKind::SlashEq:
                    return dcc::lex::TokenKind::Slash;
                case dcc::lex::TokenKind::PercentEq:
                    return dcc::lex::TokenKind::Percent;
                case dcc::lex::TokenKind::AmpEq:
                    return dcc::lex::TokenKind::Amp;
                case dcc::lex::TokenKind::PipeEq:
                    return dcc::lex::TokenKind::Pipe;
                case dcc::lex::TokenKind::CaretEq:
                    return dcc::lex::TokenKind::Caret;
                case dcc::lex::TokenKind::LtLtEq:
                    return dcc::lex::TokenKind::LtLt;
                case dcc::lex::TokenKind::GtGtEq:
                    return dcc::lex::TokenKind::GtGt;
                default:
                    return dcc::lex::TokenKind::Invalid;
            }
        }

        IrValue* lower_assign(ast::BinaryExpr const* bin, dcc::types::TypePtr sema_ty, IrType const* ir_ty)
        {
            std::ignore = ir_ty;
            auto* rhs_val = lower_implicit_enum_construction(bin->rhs, [&]() { return lower_expr(bin->rhs); });

            auto* rhs_sema_type = get_sema_resolved_type(bin->rhs);
            auto* lhs_sema_type = get_sema_resolved_type(bin->lhs);
            if (lhs_sema_type && lhs_sema_type->kind == types::TypeKind::Slice && rhs_sema_type && rhs_sema_type->kind == types::TypeKind::Array)
                rhs_val = coerce_array_to_slice(rhs_val, rhs_sema_type, lhs_sema_type);

            auto lv = lower_assign_lvalue(bin->lhs);
            if (!lv.entry && !lv.gep_ptr)
                lower_panic(bin, "assignment LHS must be assignable");

            if (lv.entry)
            {
                auto& entry = *lv.entry;
                if (!entry.is_storage)
                    lower_panic(bin, "assignment to non-storage");

                if (entry.is_volatile)
                    append_inst(m_ctx.store_volatile(rhs_val, entry.value));
                else
                    append_inst(m_ctx.store(rhs_val, entry.value));
            }
            else
            {
                append_inst(m_ctx.store(rhs_val, lv.gep_ptr));
            }

            if (sema_ty && sema_ty->kind != dcc::types::TypeKind::Void)
                return rhs_val;

            return nullptr;
        }

        IrValue* lower_compound_assign(ast::BinaryExpr const* bin, dcc::types::TypePtr sema_ty, IrType const* ir_ty)
        {
            auto lv = lower_assign_lvalue(bin->lhs);
            if (!lv.entry && !lv.gep_ptr)
                lower_panic(bin, "compound assignment LHS must be assignable");

            IrValue* ptr;
            bool is_volatile = false;

            if (lv.entry)
            {
                auto& entry = *lv.entry;
                if (!entry.is_storage)
                    lower_panic(bin, "compound assignment to non-storage");
                ptr = entry.value;
                is_volatile = entry.is_volatile;
            }
            else
                ptr = lv.gep_ptr;

            auto* loaded = m_ctx.load(ir_ty, ptr);
            auto load_name = ident_name();
            loaded->name = m_name_pool.back();
            append_inst(loaded);

            auto* rhs_val = lower_expr(bin->rhs);

            auto bin_op = compound_to_binop(bin->op);
            auto* result = make_binop(ir_ty, loaded, rhs_val, bin_op, sema_ty);
            auto result_name = ident_name();
            result->name = m_name_pool.back();
            append_inst(result);

            if (is_volatile)
                append_inst(m_ctx.store_volatile(result, ptr));
            else
                append_inst(m_ctx.store(result, ptr));

            if (sema_ty && sema_ty->kind != dcc::types::TypeKind::Void)
                return result;

            return nullptr;
        }

        struct LValueResult
        {
            MapEntry* entry{};
            IrValue* gep_ptr{};
        };

        LValueResult lower_assign_lvalue(ast::Expr const* expr)
        {
            if (expr->kind == ast::ExprKind::Ident)
            {
                auto* id = static_cast<ast::IdentExpr const*>(expr);
                auto* resolved = id->sema.resolved_decl;
                if (!resolved)
                    lower_panic(id, "IdentExpr missing resolved_decl for assignment");

                {
                    auto it = m_value_map.find(resolved);
                    if (it != m_value_map.end())
                        return {&it->second, nullptr};
                }

                {
                    auto name_it = m_named_values.find(id->name);
                    if (name_it != m_named_values.end())
                        return {&name_it->second, nullptr};
                }

                if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                {
                    auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                    if (global)
                    {
                        auto* ptr_type = m_ctx.pointer_to(global->type);
                        auto* global_ref = m_ctx.global_ref(global, ptr_type);
                        return {nullptr, global_ref};
                    }
                }

                lower_panic(id, "identifier not found for assignment");
            }

            if (expr->kind == ast::ExprKind::FieldAccess)
            {
                auto* gep = lower_field_lvalue(static_cast<ast::FieldAccessExpr const*>(expr));
                return {nullptr, gep};
            }

            if (expr->kind == ast::ExprKind::Index)
            {
                auto* gep = lower_index_lvalue(static_cast<ast::IndexExpr const*>(expr));
                return {nullptr, gep};
            }

            if (expr->kind == ast::ExprKind::PackAccess)
                lower_panic(expr, "PackAccess must be resolved before IR lowering");

            if (expr->kind == ast::ExprKind::Unary)
            {
                auto* u = static_cast<ast::UnaryExpr const*>(expr);
                if (u->op == dcc::lex::TokenKind::Star)
                {
                    auto* ptr = lower_expr(u->operand);
                    return {nullptr, ptr};
                }
            }

            return {nullptr, nullptr};
        }

        IrValue* make_binop(IrType const* ty, IrValue* lhs, IrValue* rhs, dcc::lex::TokenKind op, dcc::types::TypePtr sema_ty)
        {
            auto type_cat = classify_type(sema_ty);

            switch (op)
            {
                case dcc::lex::TokenKind::Plus:
                    return m_ctx.add(ty, lhs, rhs);
                case dcc::lex::TokenKind::Minus:
                    return m_ctx.sub(ty, lhs, rhs);
                case dcc::lex::TokenKind::Star:
                    return m_ctx.mul(ty, lhs, rhs);

                case dcc::lex::TokenKind::Slash: {
                    if (type_cat == TypeCat::Float)
                        return m_ctx.fdiv(ty, lhs, rhs);
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.udiv(ty, lhs, rhs);
                    return m_ctx.sdiv(ty, lhs, rhs);
                }

                case dcc::lex::TokenKind::Percent: {
                    if (type_cat == TypeCat::Float)
                        return m_ctx.frem(ty, lhs, rhs);
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.urem(ty, lhs, rhs);
                    return m_ctx.srem(ty, lhs, rhs);
                }

                case dcc::lex::TokenKind::Amp:
                    return m_ctx.and_(ty, lhs, rhs);
                case dcc::lex::TokenKind::Pipe:
                    return m_ctx.or_(ty, lhs, rhs);
                case dcc::lex::TokenKind::Caret:
                    return m_ctx.xor_(ty, lhs, rhs);

                case dcc::lex::TokenKind::LtLt:
                    return m_ctx.shl(ty, lhs, rhs);

                case dcc::lex::TokenKind::GtGt: {
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.lshr(ty, lhs, rhs);
                    return m_ctx.ashr(ty, lhs, rhs);
                }

                case dcc::lex::TokenKind::EqEq:
                    return m_ctx.cmp_eq(lhs, rhs);
                case dcc::lex::TokenKind::BangEq:
                    return m_ctx.cmp_ne(lhs, rhs);

                case dcc::lex::TokenKind::Lt: {
                    if (type_cat == TypeCat::Float)
                        return m_ctx.cmp_olt(lhs, rhs);
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.cmp_ult(lhs, rhs);
                    return m_ctx.cmp_lt(lhs, rhs);
                }

                case dcc::lex::TokenKind::LtEq: {
                    if (type_cat == TypeCat::Float)
                        return m_ctx.cmp_ole(lhs, rhs);
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.cmp_ule(lhs, rhs);
                    return m_ctx.cmp_le(lhs, rhs);
                }

                case dcc::lex::TokenKind::Gt: {
                    if (type_cat == TypeCat::Float)
                        return m_ctx.cmp_ogt(lhs, rhs);
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.cmp_ugt(lhs, rhs);
                    return m_ctx.cmp_gt(lhs, rhs);
                }

                case dcc::lex::TokenKind::GtEq: {
                    if (type_cat == TypeCat::Float)
                        return m_ctx.cmp_oge(lhs, rhs);
                    if (type_cat == TypeCat::Unsigned)
                        return m_ctx.cmp_uge(lhs, rhs);
                    return m_ctx.cmp_ge(lhs, rhs);
                }

                default:
                    lower_panic(std::format("unsupported binary operator: {}", static_cast<int>(op)));
            }
        }

        enum class TypeCat : std::uint8_t
        {
            Signed,
            Unsigned,
            Float,
        };

        static TypeCat classify_type(dcc::types::TypePtr t)
        {
            if (!t)
                return TypeCat::Signed;

            if (dcc::types::type_cast<dcc::types::FloatType>(t))
                return TypeCat::Float;

            if (auto* it = dcc::types::type_cast<dcc::types::IntType>(t))
                return it->is_signed ? TypeCat::Signed : TypeCat::Unsigned;

            return TypeCat::Signed;
        }

        [[nodiscard]] static types::IntType const* get_switch_int_type(dcc::types::TypePtr ty) noexcept
        {
            if (!ty)
                return nullptr;

            if (auto* it = dcc::types::type_cast<dcc::types::IntType>(ty))
                return it;

            return nullptr;
        }

        IrValue* lower_cast_expr(ast::CastExpr const* c)
        {
            if (c->sema.construction_kind == ast::ExprSema::ConstructionKind::Enum && c->sema.constructed_variant)
            {
                auto* enum_type = get_sema_resolved_type(c);
                auto* variant = c->sema.constructed_variant;

                auto* operand = lower_expr(c->operand);

                if (variant->payload.size() == 1)
                {
                    auto* payload_sema_type = variant->payload[0] ? get_canonical_type(variant->payload[0]) : nullptr;
                    if (payload_sema_type)
                    {
                        auto* src_sema_ty = get_sema_resolved_type(c->operand);
                        auto* dst_ir_ty = lower_type(payload_sema_type);
                        auto* src_ir_ty = lower_type(src_sema_ty);

                        if (src_ir_ty != dst_ir_ty)
                        {
                            if (src_ir_ty->kind == IrTypeKind::Int && dst_ir_ty->kind == IrTypeKind::Int)
                            {
                                auto* src_int = static_cast<IrIntType const*>(src_ir_ty);
                                auto* dst_int = static_cast<IrIntType const*>(dst_ir_ty);
                                if (src_int->bits < dst_int->bits)
                                {
                                    IrValue* inst = src_int->is_signed ? (IrValue*)m_ctx.sext(dst_ir_ty, operand) : (IrValue*)m_ctx.zext(dst_ir_ty, operand);
                                    auto name = ident_name();
                                    inst->name = m_name_pool.back();
                                    append_inst(inst);
                                    operand = inst;
                                }
                                else if (src_int->bits > dst_int->bits)
                                {
                                    auto* inst = m_ctx.trunc(dst_ir_ty, operand);
                                    auto name = ident_name();
                                    inst->name = m_name_pool.back();
                                    append_inst(inst);
                                    operand = inst;
                                }
                            }
                            else if (src_ir_ty->kind == IrTypeKind::Float && dst_ir_ty->kind == IrTypeKind::Float)
                            {
                                auto* src_ft = static_cast<IrFloatType const*>(src_ir_ty);
                                auto* dst_ft = static_cast<IrFloatType const*>(dst_ir_ty);
                                if (src_ft->bits < dst_ft->bits)
                                {
                                    auto* inst = m_ctx.fpext(dst_ir_ty, operand);
                                    auto name = ident_name();
                                    inst->name = m_name_pool.back();
                                    append_inst(inst);
                                    operand = inst;
                                }
                                else
                                {
                                    auto* inst = m_ctx.fptrunc(dst_ir_ty, operand);
                                    auto name = ident_name();
                                    inst->name = m_name_pool.back();
                                    append_inst(inst);
                                    operand = inst;
                                }
                            }
                            else if (src_ir_ty->kind == IrTypeKind::Float && dst_ir_ty->kind == IrTypeKind::Int)
                            {
                                auto* inst = m_ctx.fptoi(dst_ir_ty, operand);
                                auto name = ident_name();
                                inst->name = m_name_pool.back();
                                append_inst(inst);
                                operand = inst;
                            }
                            else if (src_ir_ty->kind == IrTypeKind::Int && dst_ir_ty->kind == IrTypeKind::Float)
                            {
                                auto* inst = m_ctx.itofp(dst_ir_ty, operand);
                                auto name = ident_name();
                                inst->name = m_name_pool.back();
                                append_inst(inst);
                                operand = inst;
                            }
                        }

                        std::vector<IrValue*> payload_args{operand};
                        return lower_tagged_enum_construction(variant, enum_type, payload_args);
                    }
                }

                return lower_tagged_enum_construction(variant, enum_type, std::span<IrValue*>{});
            }

            auto* dst_sema_ty = get_sema_resolved_type(c);
            auto* src_sema_ty = get_sema_resolved_type(c->operand);
            auto* dst_ir_ty = lower_type(dst_sema_ty);
            auto* src_ir_ty = lower_type(src_sema_ty);

            if (src_ir_ty == dst_ir_ty)
                return lower_expr(c->operand);

            auto* operand = lower_expr(c->operand);

            if (dst_sema_ty && dst_sema_ty->kind == dcc::types::TypeKind::Bool)
            {
                if (src_sema_ty && src_sema_ty->kind == dcc::types::TypeKind::NullT)
                {
                    auto* zero = m_ctx.null_const(src_ir_ty);
                    auto* inst = m_ctx.cmp_ne(operand, zero);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }

                if (src_sema_ty && src_sema_ty->kind == dcc::types::TypeKind::Pointer)
                {
                    auto* null = m_ctx.null_const(src_ir_ty);
                    auto* inst = m_ctx.cmp_ne(operand, null);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }

                auto* zero = m_ctx.int_const(src_ir_ty, 0);
                auto* inst = m_ctx.cmp_ne(operand, zero);
                auto name = ident_name();
                inst->name = m_name_pool.back();
                append_inst(inst);

                return inst;
            }

            if (src_ir_ty->kind == IrTypeKind::Int && dst_ir_ty->kind == IrTypeKind::Int)
            {
                auto* src_int = static_cast<IrIntType const*>(src_ir_ty);
                auto* dst_int = static_cast<IrIntType const*>(dst_ir_ty);

                if (src_int->bits < dst_int->bits)
                {
                    IrValue* inst = src_int->is_signed ? (IrValue*)m_ctx.sext(dst_ir_ty, operand) : (IrValue*)m_ctx.zext(dst_ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }
                else if (src_int->bits > dst_int->bits)
                {
                    auto* inst = m_ctx.trunc(dst_ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }
                else
                {
                    auto* inst = m_ctx.bitcast(dst_ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }
            }

            if (src_ir_ty->kind == IrTypeKind::Float && dst_ir_ty->kind == IrTypeKind::Float)
            {
                auto* src_ft = static_cast<IrFloatType const*>(src_ir_ty);
                auto* dst_ft = static_cast<IrFloatType const*>(dst_ir_ty);

                if (src_ft->bits < dst_ft->bits)
                {
                    auto* inst = m_ctx.fpext(dst_ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }
                else
                {
                    auto* inst = m_ctx.fptrunc(dst_ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }
            }

            if (src_ir_ty->kind == IrTypeKind::Float && dst_ir_ty->kind == IrTypeKind::Int)
            {
                auto* inst = m_ctx.fptoi(dst_ir_ty, operand);
                auto name = ident_name();
                inst->name = m_name_pool.back();
                append_inst(inst);

                return inst;
            }

            if (src_ir_ty->kind == IrTypeKind::Int && dst_ir_ty->kind == IrTypeKind::Float)
            {
                auto* inst = m_ctx.itofp(dst_ir_ty, operand);
                auto name = ident_name();
                inst->name = m_name_pool.back();
                append_inst(inst);

                return inst;
            }

            if (src_ir_ty->kind == IrTypeKind::Pointer && dst_ir_ty->kind == IrTypeKind::Int)
            {
                auto* inst = m_ctx.ptrtoi(dst_ir_ty, operand);
                auto name = ident_name();
                inst->name = m_name_pool.back();
                append_inst(inst);

                return inst;
            }

            if (src_ir_ty->kind == IrTypeKind::Int && dst_ir_ty->kind == IrTypeKind::Pointer)
            {
                auto* inst = m_ctx.itoptr(dst_ir_ty, operand);
                auto name = ident_name();
                inst->name = m_name_pool.back();
                append_inst(inst);

                return inst;
            }

            if (src_ir_ty->kind == IrTypeKind::Pointer && dst_ir_ty->kind == IrTypeKind::Pointer)
            {
                auto* src_ptr = static_cast<IrPointerType const*>(src_ir_ty);
                auto* dst_ptr = static_cast<IrPointerType const*>(dst_ir_ty);

                if (src_ptr->seg != dst_ptr->seg)
                {
                    auto* inst = m_ctx.segcast(dst_ir_ty, operand);
                    auto name = ident_name();
                    inst->name = m_name_pool.back();
                    append_inst(inst);

                    return inst;
                }

                auto* inst = m_ctx.bitcast(dst_ir_ty, operand);
                auto name = ident_name();
                inst->name = m_name_pool.back();
                append_inst(inst);

                return inst;
            }

            if (src_ir_ty->kind == IrTypeKind::Array && dst_ir_ty->kind == IrTypeKind::Pointer)
            {
                IrValue* arr_ptr = nullptr;

                if (c->operand->kind == ast::ExprKind::Ident)
                {
                    auto const* id = static_cast<ast::IdentExpr const*>(c->operand);
                    if (auto* resolved = id->sema.resolved_decl)
                    {
                        auto it = m_value_map.find(resolved);
                        if (it != m_value_map.end() && it->second.is_storage)
                            arr_ptr = it->second.value;
                    }
                }

                if (!arr_ptr)
                {
                    auto* arr_ptr_type = m_ctx.pointer_to(src_ir_ty);
                    auto* temp = m_ctx.alloca(arr_ptr_type, src_ir_ty);
                    auto temp_name = ident_name();
                    temp->name = m_name_pool.back();
                    append_inst(temp);
                    append_inst(m_ctx.store(operand, temp));
                    arr_ptr = temp;
                }

                auto const* ir_arr = static_cast<IrArrayType const*>(src_ir_ty);
                auto* ir_elem_type = ir_arr->element;

                auto* gep = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), arr_ptr);
                gep->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(m_ctx.int_t(64, false), 0), 0});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);

                if (gep->type != dst_ir_ty)
                {
                    auto* bc = m_ctx.bitcast(dst_ir_ty, gep);
                    auto bc_name = ident_name();
                    bc->name = m_name_pool.back();
                    append_inst(bc);
                    return bc;
                }

                return gep;
            }

            if (src_ir_ty->kind == IrTypeKind::Slice && dst_ir_ty->kind == IrTypeKind::Pointer)
            {
                auto* ptr_extract = m_ctx.extract(dst_ir_ty, operand, 0);
                auto name = ident_name();
                ptr_extract->name = m_name_pool.back();
                append_inst(ptr_extract);
                return ptr_extract;
            }

            auto* inst = m_ctx.bitcast(dst_ir_ty, operand);
            auto name = ident_name();
            inst->name = m_name_pool.back();
            append_inst(inst);

            return inst;
        }

        IrValue* lower_if_expr(ast::IfExpr const* ie)
        {
            auto* sema_ty = get_sema_resolved_type(ie);
            bool has_result = sema_ty && sema_ty->kind != dcc::types::TypeKind::Void;
            auto* ir_result_ty = has_result ? lower_type(sema_ty) : nullptr;

            auto* then_bb = create_block("if.then");
            auto* else_bb = create_block("if.else");
            auto* merge_bb = create_block("if.merge");

            auto* cond_val = lower_expr(ie->condition);
            emit_br_cond(cond_val, then_bb, else_bb);

            set_current_block(then_bb);
            IrValue* then_val = nullptr;
            {
                push_scope();

                for (auto* stmt : ie->then_block.stmts)
                    lower_stmt(stmt);

                if (ie->then_block.tail && !current_block_terminated())
                    then_val = lower_expr(ie->then_block.tail);

                pop_scope();
            }
            bool then_terminated = current_block_terminated();
            IrBasicBlock* then_exit = then_terminated ? nullptr : m_current_block;

            if (!then_terminated)
                emit_br(merge_bb);

            set_current_block(else_bb);
            IrValue* else_val = nullptr;
            if (ie->else_branch)
            {
                if (ie->else_branch->kind == ast::ExprKind::Block)
                {
                    auto* block_expr = static_cast<ast::BlockExpr const*>(ie->else_branch);
                    push_scope();

                    for (auto* stmt : block_expr->body.stmts)
                        lower_stmt(stmt);

                    if (block_expr->body.tail && !current_block_terminated())
                        else_val = lower_expr(block_expr->body.tail);

                    pop_scope();
                }
                else if (ie->else_branch->kind == ast::ExprKind::If)
                {
                    push_scope();
                    else_val = lower_if_expr(static_cast<ast::IfExpr const*>(ie->else_branch));
                    pop_scope();
                }
                else
                {
                    push_scope();
                    else_val = lower_expr(ie->else_branch);
                    pop_scope();
                }
            }
            bool else_terminated = current_block_terminated();
            IrBasicBlock* else_exit = else_terminated ? nullptr : m_current_block;

            if (!else_terminated)
                emit_br(merge_bb);

            int incoming_count = (then_terminated ? 0 : 1) + (else_terminated ? 0 : 1);

            if (incoming_count == 0)
                return has_result ? m_ctx.int_const(ir_result_ty, 0) : nullptr;

            set_current_block(merge_bb);

            if (!has_result)
                return nullptr;

            if (incoming_count == 1)
            {
                if (!then_terminated)
                    return then_val ? then_val : m_ctx.int_const(ir_result_ty, 0);
                else
                    return else_val ? else_val : m_ctx.int_const(ir_result_ty, 0);
            }

            auto* phi = emit_phi(ir_result_ty);
            add_phi_incoming(phi, then_val ? then_val : m_ctx.int_const(ir_result_ty, 0),
                             then_exit ? then_exit : then_bb);
            add_phi_incoming(phi, else_val ? else_val : m_ctx.int_const(ir_result_ty, 0),
                             else_exit ? else_exit : else_bb);
            return phi;
        }

        static std::optional<std::int64_t> pattern_int_value(ast::Expr const* expr)
        {
            if (!expr)
                return std::nullopt;
            if (auto* il = ast::node_cast<ast::IntLiteralExpr>(expr))
                return il->value;
            if (auto* cl = ast::node_cast<ast::CharLiteralExpr>(expr))
                return static_cast<std::int64_t>(cl->codepoint);
            if (auto* ucl = ast::node_cast<ast::U16CharLiteralExpr>(expr))
                return static_cast<std::int64_t>(ucl->value);
            return std::nullopt;
        }

        static bool flatten_switch_pattern(ast::Pattern const* pat, std::vector<std::pair<std::int64_t, std::int64_t>>& out, types::IntType const* ty = nullptr)
        {
            if (!pat)
                return false;

            switch (pat->kind)
            {
                case ast::PatternKind::Literal: {
                    auto const& lp = static_cast<ast::LiteralPattern const&>(*pat);
                    auto v = pattern_int_value(lp.value);
                    if (!v)
                        return false;
                    out.push_back({*v, *v});
                    return true;
                }
                case ast::PatternKind::Range: {
                    auto const& rp = static_cast<ast::RangePattern const&>(*pat);
                    auto lo = pattern_int_value(rp.start);
                    auto hi = pattern_int_value(rp.end);
                    if (!lo || !hi)
                        return false;

                    if (ty)
                    {
                        auto ord_lo = int_domain::to_ordinal(*lo, *ty);
                        auto ord_hi = int_domain::to_ordinal(*hi, *ty);

                        if (rp.inclusive)
                        {
                            if (ord_hi < ord_lo)
                                return false;
                            out.push_back({int_domain::ordinal_to_raw_bits(ord_lo, *ty), int_domain::ordinal_to_raw_bits(ord_hi, *ty)});
                        }
                        else
                        {
                            if (ord_hi <= ord_lo)
                                return false;
                            out.push_back({int_domain::ordinal_to_raw_bits(ord_lo, *ty), int_domain::ordinal_to_raw_bits(ord_hi - 1, *ty)});
                        }
                    }
                    else
                    {
                        if (rp.inclusive)
                            out.push_back({*lo, *hi});
                        else
                        {
                            if (*hi <= *lo)
                                return false;
                            out.push_back({*lo, *hi - 1});
                        }
                    }
                    return true;
                }
                case ast::PatternKind::Or: {
                    auto const& op = static_cast<ast::OrPattern const&>(*pat);
                    for (auto const& alt : op.alternatives)
                        if (!flatten_switch_pattern(alt, out, ty))
                            return false;

                    return true;
                }
                case ast::PatternKind::EnumDestructure: {
                    auto const& ep = static_cast<ast::EnumDestructurePattern const&>(*pat);
                    if (!ep.resolved_variant)
                        return false;

                    for (auto* sub : ep.payload)
                    {
                        if (!sub)
                            continue;

                        if (sub->kind == ast::PatternKind::Wildcard || sub->kind == ast::PatternKind::Binding)
                            continue;

                        return false;
                    }

                    std::int64_t disc = static_cast<std::int64_t>(ep.resolved_variant->discriminant);
                    out.push_back({disc, disc});
                    return true;
                }
                case ast::PatternKind::Ref: {
                    auto const& rp = static_cast<ast::RefPattern const&>(*pat);
                    if (rp.inner)
                        return flatten_switch_pattern(rp.inner, out, ty);
                    return false;
                }
                default:
                    return false;
            }
        }

        static bool has_by_ref_binding(ast::Pattern const* pat)
        {
            if (!pat)
                return false;

            switch (pat->kind)
            {
                case ast::PatternKind::Binding:
                    return static_cast<ast::BindingPattern const*>(pat)->by_reference;
                case ast::PatternKind::Ref:
                    return true;
                case ast::PatternKind::Or: {
                    auto const& op = static_cast<ast::OrPattern const&>(*pat);
                    for (auto* alt : op.alternatives)
                        if (has_by_ref_binding(alt))
                            return true;
                    return false;
                }
                case ast::PatternKind::EnumDestructure: {
                    auto const& ep = static_cast<ast::EnumDestructurePattern const&>(*pat);
                    for (auto* sub : ep.payload)
                        if (has_by_ref_binding(sub))
                            return true;
                    return false;
                }
                case ast::PatternKind::StructDestructure: {
                    auto const& sp = static_cast<ast::StructDestructurePattern const&>(*pat);
                    for (auto const& field : sp.fields)
                        if (has_by_ref_binding(field.pattern))
                            return true;
                    return false;
                }
                default:
                    return false;
            }
        }

        IrValue* lower_match_expr(ast::MatchExpr const* me)
        {
            auto* sema_ty = get_sema_resolved_type(me);
            bool has_result = sema_ty && sema_ty->kind != dcc::types::TypeKind::Void;
            auto* ir_result_ty = has_result ? lower_type(sema_ty) : nullptr;
            bool is_void = !has_result;

            auto* operand_val = lower_expr(me->operand);
            auto* operand_sema_ty = get_sema_resolved_type(me->operand);

            auto* merge_bb = create_block("match.merge");
            auto* fail_bb = create_block("match.fail");

            std::size_t arm_count = me->arms.size();

            struct ArmBlocks
            {
                IrBasicBlock* test{};
                IrBasicBlock* body_start{};
                IrBasicBlock* exit_block{};
                IrValue* result{};
                bool terminated{};
            };
            std::vector<ArmBlocks> arm_blocks(arm_count);

            for (std::size_t i = 0; i < arm_count; ++i)
            {
                auto label = std::string("match.arm") + std::to_string(i);
                arm_blocks[i].test = create_block(label + ".test");
                arm_blocks[i].body_start = create_block(label + ".body");
            }

            bool use_switch = false;
            auto op_type_cat = classify_type(operand_sema_ty);

            auto* switch_ty = get_switch_int_type(operand_sema_ty);

            if (op_type_cat != TypeCat::Float)
            {
                struct SwitchInterval
                {
                    std::uint64_t start;
                    std::uint64_t end;
                    std::size_t arm_index;
                };
                std::vector<SwitchInterval> intervals;
                bool switch_ok = true;

                for (std::size_t i = 0; i < arm_count && switch_ok; ++i)
                {
                    auto const& arm = me->arms[i];

                    if (arm.guard)
                    {
                        switch_ok = false;
                        break;
                    }

                    bool is_default_arm = false;
                    if (!arm.pattern)
                    {
                        is_default_arm = true;
                    }
                    else if (arm.pattern->kind == ast::PatternKind::Wildcard)
                    {
                        is_default_arm = true;
                    }
                    else if (arm.pattern->kind == ast::PatternKind::Binding)
                    {
                        is_default_arm = true;
                    }

                    if (is_default_arm)
                    {
                        if (i != arm_count - 1)
                        {
                            switch_ok = false;
                            break;
                        }
                        continue;
                    }

                    std::vector<std::pair<std::int64_t, std::int64_t>> arm_intervals;
                    if (!flatten_switch_pattern(arm.pattern, arm_intervals, switch_ty))
                    {
                        switch_ok = false;
                        break;
                    }
                    for (auto& iv : arm_intervals)
                    {
                        if (switch_ty)
                            intervals.push_back({int_domain::to_ordinal(iv.first, *switch_ty), int_domain::to_ordinal(iv.second, *switch_ty), i});
                        else
                            intervals.push_back({static_cast<std::uint64_t>(iv.first), static_cast<std::uint64_t>(iv.second), i});
                    }
                }

                if (switch_ok && intervals.size() >= 4)
                {
                    std::ranges::sort(intervals, {}, &SwitchInterval::start);

                    bool overlap = false;
                    for (std::size_t i = 1; i < intervals.size(); ++i)
                    {
                        if (intervals[i].start <= intervals[i - 1].end)
                        {
                            overlap = true;
                            break;
                        }
                    }

                    if (!overlap)
                    {
                        auto span_lo = intervals.front().start;
                        auto span_hi = intervals.back().end;
                        auto density_limit = static_cast<std::uint64_t>(std::max<std::int64_t>(16, static_cast<std::int64_t>(intervals.size()) * 3));

                        bool dense_enough = true;

                        if (density_limit == 0)
                            dense_enough = (span_hi == span_lo);
                        else if (span_hi - span_lo <= density_limit - 1)
                            dense_enough = true;
                        else
                            dense_enough = false;

                        if (dense_enough)
                            use_switch = true;
                    }
                }
            }

            if (use_switch)
            {
                bool is_tagged_enum = false;
                types::EnumType const* enum_et = nullptr;
                if (operand_sema_ty)
                {
                    enum_et = types::type_cast<types::EnumType>(operand_sema_ty);
                    if (enum_et && enum_et->is_tagged && enum_et->tagged_layout)
                        is_tagged_enum = true;
                }

                IrValue* switch_val = operand_val;

                if (is_tagged_enum)
                {
                    auto* layout = enum_et->tagged_layout;
                    auto* ir_disc_type = m_ctx.int_t(static_cast<std::uint8_t>(layout->discriminant_type->bits), layout->discriminant_type->is_signed);
                    if (operand_ir_is_aggregate(operand_val->type))
                    {
                        auto* extract = m_ctx.extract(ir_disc_type, operand_val, 0);
                        auto name = ident_name();
                        extract->name = m_name_pool.back();
                        append_inst(extract);
                        switch_val = extract;
                    }
                }

                struct SwitchInterval
                {
                    std::uint64_t start;
                    std::uint64_t end;
                    std::size_t arm_index;
                };
                std::vector<SwitchInterval> intervals;
                bool has_switch_default = false;
                std::size_t default_arm_index = 0;

                auto* switch_emit_ty = switch_ty;
                if (is_tagged_enum && enum_et && enum_et->tagged_layout && enum_et->tagged_layout->discriminant_type)
                    switch_emit_ty = enum_et->tagged_layout->discriminant_type;

                for (std::size_t i = 0; i < arm_count; ++i)
                {
                    auto const& arm = me->arms[i];
                    if (!arm.pattern || arm.pattern->kind == ast::PatternKind::Wildcard || arm.pattern->kind == ast::PatternKind::Binding)
                    {
                        has_switch_default = true;
                        default_arm_index = i;
                        continue;
                    }
                    std::vector<std::pair<std::int64_t, std::int64_t>> arm_intervals;
                    flatten_switch_pattern(arm.pattern, arm_intervals, switch_emit_ty);
                    for (auto& iv : arm_intervals)
                    {
                        if (switch_emit_ty)
                            intervals.push_back({int_domain::to_ordinal(iv.first, *switch_emit_ty), int_domain::to_ordinal(iv.second, *switch_emit_ty), i});
                        else
                            intervals.push_back({static_cast<std::uint64_t>(iv.first), static_cast<std::uint64_t>(iv.second), i});
                    }
                }

                std::ranges::sort(intervals, {}, &SwitchInterval::start);
                std::vector<SwitchInterval> merged;
                for (std::size_t i = 0; i < intervals.size();)
                {
                    auto start = intervals[i].start;
                    auto end = intervals[i].end;
                    auto target = intervals[i].arm_index;
                    std::size_t j = i + 1;
                    while (j < intervals.size() && intervals[j].arm_index == target && intervals[j].start == end + 1)
                    {
                        end = intervals[j].end;
                        ++j;
                    }
                    merged.push_back({start, end, target});
                    i = j;
                }

                auto* default_target = has_switch_default ? arm_blocks[default_arm_index].body_start : fail_bb;

                emit_switch(switch_val, default_target);
                auto* sw = static_cast<IrSwitchInst*>(m_current_block->terminator);
                for (auto& iv : merged)
                {
                    std::int64_t case_start;
                    std::int64_t case_end;
                    if (switch_emit_ty)
                    {
                        case_start = int_domain::ordinal_to_raw_bits(iv.start, *switch_emit_ty);
                        case_end = int_domain::ordinal_to_raw_bits(iv.end, *switch_emit_ty);
                    }
                    else
                    {
                        case_start = static_cast<std::int64_t>(iv.start);
                        case_end = static_cast<std::int64_t>(iv.end);
                    }
                    sw->cases.push_back({case_start, case_end, arm_blocks[iv.arm_index].body_start});
                }

                for (std::size_t i = 0; i < arm_count; ++i)
                {
                    set_current_block(arm_blocks[i].body_start);
                    push_scope();

                    auto const& arm = me->arms[i];

                    if (is_tagged_enum && arm.pattern && arm.pattern->kind == ast::PatternKind::EnumDestructure)
                    {
                        auto const& ep = static_cast<ast::EnumDestructurePattern const&>(*arm.pattern);
                        auto* layout = enum_et->tagged_layout;
                        auto* ed = reinterpret_cast<ast::EnumDecl const*>(enum_et->decl);
                        if (ed && ep.resolved_variant)
                        {
                            auto variant_index = static_cast<std::size_t>(ep.resolved_variant - ed->variants.data());

                            for (std::size_t pi = 0; pi < ep.payload.size(); ++pi)
                            {
                                if (!ep.payload[pi])
                                    continue;

                                if (ep.payload[pi]->kind == ast::PatternKind::Wildcard)
                                    continue;

                                auto* concrete_payload_type =
                                    (variant_index < layout->variant_count) ? layout->variants[variant_index].variant_payload_type_or_null : nullptr;
                                auto* payload_sema_type = (pi < ep.resolved_variant->payload.size() && ep.resolved_variant->payload[pi])
                                                              ? get_canonical_type(ep.resolved_variant->payload[pi])
                                                              : nullptr;
                                auto* payload_sema_canon = concrete_payload_type ? concrete_payload_type : payload_sema_type;
                                if (!payload_sema_canon)
                                    continue;

                                auto* ir_payload_type = lower_type(payload_sema_canon);
                                auto* ir_byte = m_ctx.int_t(8, false);
                                auto* ir_payload_arr_ty = m_ctx.array_t(ir_byte, layout->payload_size);

                                IrValue* payload_arr = nullptr;
                                if (operand_ir_is_aggregate(operand_val->type))
                                {
                                    auto* extract = m_ctx.extract(ir_payload_arr_ty, operand_val, 1);
                                    auto name = ident_name();
                                    extract->name = m_name_pool.back();
                                    append_inst(extract);
                                    payload_arr = extract;
                                }
                                else
                                    payload_arr = operand_val;

                                auto* payload_buf = m_ctx.alloca(m_ctx.pointer_to(ir_payload_arr_ty), ir_payload_arr_ty);
                                auto buf_name = ident_name();
                                payload_buf->name = m_name_pool.back();
                                append_inst(payload_buf);
                                append_inst(m_ctx.store(payload_arr, payload_buf));

                                auto* typed_ptr = m_ctx.bitcast(m_ctx.pointer_to(ir_payload_type), payload_buf);
                                auto ptr_name = ident_name();
                                typed_ptr->name = m_name_pool.back();
                                append_inst(typed_ptr);
                                auto* payload_val = m_ctx.load(ir_payload_type, typed_ptr);
                                auto load_name = ident_name();
                                payload_val->name = m_name_pool.back();
                                append_inst(payload_val);

                                if (ep.payload[pi]->kind == ast::PatternKind::Binding)
                                {
                                    auto const& bp = static_cast<ast::BindingPattern const&>(*ep.payload[pi]);
                                    if (bp.by_reference)
                                    {
                                        if (operand_ir_is_aggregate(operand_val->type))
                                        {
                                            auto* base_ptr = lower_addr_of(me->operand);
                                            auto* gep = m_ctx.gep(m_ctx.pointer_to(ir_payload_arr_ty), base_ptr);
                                            gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, 1});
                                            auto gep_name = ident_name();
                                            gep->name = m_name_pool.back();
                                            append_inst(gep);
                                            auto* payload_ptr = m_ctx.bitcast(m_ctx.pointer_to(ir_payload_type), gep);
                                            auto payload_ptr_name = ident_name();
                                            payload_ptr->name = m_name_pool.back();
                                            append_inst(payload_ptr);
                                            auto* ptr_sema_ty = make_ptr_sema_type(payload_sema_canon, dcc::types::Qual::None);
                                            m_named_values[bp.name] = MapEntry{payload_ptr, false, ptr_sema_ty, false};
                                        }
                                        else
                                        {
                                            auto* ptr_sema_ty = make_ptr_sema_type(payload_sema_canon, dcc::types::Qual::None);
                                            m_named_values[bp.name] = MapEntry{payload_buf, false, ptr_sema_ty, false};
                                        }
                                    }
                                    else if (operand_ir_is_aggregate(ir_payload_type))
                                    {
                                        auto* ptr_type = m_ctx.pointer_to(ir_payload_type, ir::Segment::None);
                                        auto* sub_alloca = m_ctx.alloca(ptr_type, ir_payload_type);
                                        auto sub_alloca_name = ident_name();
                                        sub_alloca->name = m_name_pool.back();
                                        append_inst(sub_alloca);
                                        append_inst(m_ctx.store(payload_val, sub_alloca));
                                        m_named_values[bp.name] = MapEntry{sub_alloca, true, payload_sema_canon, false};
                                    }
                                    else
                                        m_named_values[bp.name] = MapEntry{payload_val, false, payload_sema_canon, false};
                                }
                            }
                        }
                    }
                    else if (arm.pattern && arm.pattern->kind == ast::PatternKind::Binding)
                    {
                        auto const& bp = static_cast<ast::BindingPattern const&>(*arm.pattern);
                        if (bp.by_reference)
                        {
                            auto* ptr = lower_addr_of(me->operand);
                            auto* ptr_sema_ty = make_ptr_sema_type(operand_sema_ty, get_sema_pointee_quals(operand_sema_ty));
                            m_named_values[bp.name] = MapEntry{ptr, false, ptr_sema_ty, false};
                        }
                        else
                            m_named_values[bp.name] = MapEntry{operand_val, false, operand_sema_ty, false};
                    }

                    if (arm.body)
                        arm_blocks[i].result = lower_expr(arm.body);

                    arm_blocks[i].terminated = current_block_terminated();

                    if (!arm_blocks[i].terminated)
                    {
                        arm_blocks[i].exit_block = m_current_block;
                        emit_br(merge_bb);
                    }

                    pop_scope();
                }
            }
            else
            {
                emit_br(arm_blocks[0].test);

                for (std::size_t i = 0; i < arm_count; ++i)
                {
                    set_current_block(arm_blocks[i].test);
                    push_scope();

                    auto const& arm = me->arms[i];
                    auto* next_test = (i + 1 < arm_count) ? arm_blocks[i + 1].test : fail_bb;

                    if (arm.pattern)
                    {
                        bool matched = lower_pattern_test(operand_val, operand_sema_ty, *arm.pattern, arm_blocks[i].body_start, next_test, me->operand);
                        std::ignore = matched;
                    }
                    else
                    {
                        emit_br(arm_blocks[i].body_start);
                    }

                    set_current_block(arm_blocks[i].body_start);

                    if (arm.guard)
                    {
                        auto* guard_bb = create_block(std::string("match.guard") + std::to_string(i) + ".pass");
                        auto* guard_val = lower_expr(arm.guard);
                        emit_br_cond(guard_val, guard_bb, next_test);

                        set_current_block(guard_bb);
                    }

                    if (arm.body)
                    {
                        arm_blocks[i].result = lower_expr(arm.body);
                    }
                    arm_blocks[i].terminated = current_block_terminated();

                    if (!arm_blocks[i].terminated)
                    {
                        arm_blocks[i].exit_block = m_current_block;
                        emit_br(merge_bb);
                    }

                    pop_scope();
                }
            }

            set_current_block(fail_bb);
            emit_unreachable();

            set_current_block(merge_bb);

            if (is_void)
                return nullptr;

            int incoming_count = 0;
            for (std::size_t i = 0; i < arm_count; ++i)
                if (!arm_blocks[i].terminated)
                    ++incoming_count;

            if (incoming_count == 0)
                return m_ctx.int_const(ir_result_ty, 0);

            if (incoming_count == 1)
            {
                for (std::size_t i = 0; i < arm_count; ++i)
                    if (!arm_blocks[i].terminated && arm_blocks[i].result)
                        return arm_blocks[i].result;
                return m_ctx.int_const(ir_result_ty, 0);
            }

            auto* phi = emit_phi(ir_result_ty);
            for (std::size_t i = 0; i < arm_count; ++i)
                if (!arm_blocks[i].terminated)
                    add_phi_incoming(phi, arm_blocks[i].result ? arm_blocks[i].result : m_ctx.int_const(ir_result_ty, 0),
                                     arm_blocks[i].exit_block ? arm_blocks[i].exit_block : arm_blocks[i].body_start);

            return phi;
        }

        bool lower_pattern_test(IrValue* value, dcc::types::TypePtr sema_ty, ast::Pattern const& pattern, IrBasicBlock* pass_bb, IrBasicBlock* fail_bb,
                                ast::Expr const* operand_expr)
        {
            switch (pattern.kind)
            {
                case ast::PatternKind::Wildcard:
                case ast::PatternKind::Binding: {
                    if (pattern.kind == ast::PatternKind::Binding)
                    {
                        auto const& bp = static_cast<ast::BindingPattern const&>(pattern);
                        auto* ir_ty = lower_type(sema_ty);

                        if (bp.by_reference)
                        {
                            if (operand_expr)
                            {
                                auto* ptr = lower_addr_of(operand_expr);
                                auto* ptr_sema_ty = make_ptr_sema_type(sema_ty, get_sema_pointee_quals(sema_ty));
                                m_named_values[bp.name] = MapEntry{ptr, false, ptr_sema_ty, false};
                            }
                            else
                            {
                                auto* ptr_type = m_ctx.pointer_to(ir_ty, ir::Segment::None);
                                auto* alloca = m_ctx.alloca(ptr_type, ir_ty);
                                auto name = ident_name();
                                alloca->name = m_name_pool.back();
                                append_inst(alloca);
                                append_inst(m_ctx.store(value, alloca));
                                auto* ptr_sema_ty = make_ptr_sema_type(sema_ty, get_sema_pointee_quals(sema_ty));
                                m_named_values[bp.name] = MapEntry{alloca, false, ptr_sema_ty, false};
                            }
                        }
                        else if (operand_ir_is_aggregate(ir_ty))
                        {
                            auto* ptr_type = m_ctx.pointer_to(ir_ty, ir::Segment::None);
                            auto* alloca = m_ctx.alloca(ptr_type, ir_ty);
                            auto name = ident_name();
                            alloca->name = m_name_pool.back();
                            append_inst(alloca);
                            append_inst(m_ctx.store(value, alloca));
                            m_named_values[bp.name] = MapEntry{alloca, true, sema_ty, false};
                        }
                        else
                        {
                            m_named_values[bp.name] = MapEntry{value, false, sema_ty, false};
                        }
                    }
                    emit_br(pass_bb);
                    return true;
                }

                case ast::PatternKind::Ref: {
                    auto const& rp = static_cast<ast::RefPattern const&>(pattern);
                    if (rp.inner)
                        return lower_pattern_test(value, sema_ty, *rp.inner, pass_bb, fail_bb, operand_expr);
                    emit_br(fail_bb);
                    return false;
                }

                case ast::PatternKind::Literal: {
                    auto const& lp = static_cast<ast::LiteralPattern const&>(pattern);
                    if (!lp.value)
                    {
                        emit_br(fail_bb);
                        return false;
                    }
                    auto* lit_val = lower_expr(lp.value);
                    auto* cmp = emit_cmp_eq(value, lit_val, sema_ty);
                    emit_br_cond(cmp, pass_bb, fail_bb);
                    return true;
                }

                case ast::PatternKind::Range: {
                    auto const& rp = static_cast<ast::RangePattern const&>(pattern);
                    if (!rp.start || !rp.end)
                    {
                        emit_br(fail_bb);
                        return false;
                    }
                    auto* lo_val = lower_expr(rp.start);
                    auto* hi_val = lower_expr(rp.end);

                    auto* cmp_ge = emit_cmp_ge(value, lo_val, sema_ty);
                    auto* cmp_le = rp.inclusive ? emit_cmp_le(value, hi_val, sema_ty) : emit_cmp_lt(value, hi_val, sema_ty);

                    auto* and_val = m_ctx.and_(cmp_ge->type, cmp_ge, cmp_le);
                    auto name = ident_name();
                    and_val->name = m_name_pool.back();
                    append_inst(and_val);

                    emit_br_cond(and_val, pass_bb, fail_bb);
                    return true;
                }

                case ast::PatternKind::Or: {
                    auto const& op = static_cast<ast::OrPattern const&>(pattern);
                    if (op.alternatives.empty())
                    {
                        emit_br(fail_bb);
                        return false;
                    }
                    if (op.alternatives.size() == 1)
                        return lower_pattern_test(value, sema_ty, *op.alternatives[0], pass_bb, fail_bb, operand_expr);

                    auto* or_fail = create_block("match.or.fail");
                    for (std::size_t i = 0; i < op.alternatives.size(); ++i)
                    {
                        auto* next = (i + 1 < op.alternatives.size()) ? create_block("match.or.next") : or_fail;
                        if (!lower_pattern_test(value, sema_ty, *op.alternatives[i], pass_bb, next, operand_expr))
                            return false;
                        set_current_block(next);
                    }
                    set_current_block(or_fail);
                    emit_br(fail_bb);
                    return true;
                }

                case ast::PatternKind::EnumDestructure: {
                    auto const& ep = static_cast<ast::EnumDestructurePattern const&>(pattern);
                    if (!ep.resolved_variant)
                    {
                        emit_br(fail_bb);
                        return false;
                    }

                    auto const* et = dcc::types::type_cast<dcc::types::EnumType>(sema_ty);
                    if (!et)
                    {
                        emit_br(fail_bb);
                        return false;
                    }

                    auto const* enum_decl = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                    if (!enum_decl)
                    {
                        emit_br(fail_bb);
                        return false;
                    }

                    bool is_tagged = et->is_tagged && et->tagged_layout != nullptr;

                    if (is_tagged)
                    {
                        auto* layout = et->tagged_layout;

                        auto* ir_disc_type = m_ctx.int_t(static_cast<std::uint8_t>(layout->discriminant_type->bits), layout->discriminant_type->is_signed);
                        IrValue* disc_value = nullptr;
                        if (operand_ir_is_aggregate(value->type))
                        {
                            auto* extract = m_ctx.extract(ir_disc_type, value, 0);
                            auto name = ident_name();
                            extract->name = m_name_pool.back();
                            append_inst(extract);
                            disc_value = extract;
                        }
                        else
                        {
                            disc_value = value;
                        }

                        bool has_nontrivial_payload = false;
                        if (!ep.payload.empty())
                        {
                            for (auto* sub : ep.payload)
                            {
                                if (sub && sub->kind != ast::PatternKind::Wildcard && sub->kind != ast::PatternKind::Binding)
                                {
                                    has_nontrivial_payload = true;
                                    break;
                                }
                            }
                        }

                        IrBasicBlock* payload_block = nullptr;
                        IrBasicBlock* effective_pass = pass_bb;

                        if (has_nontrivial_payload)
                        {
                            payload_block = create_block("match.payload.check");
                            effective_pass = payload_block;
                        }

                        auto* disc_val = m_ctx.int_const(ir_disc_type, static_cast<std::int64_t>(ep.resolved_variant->discriminant));
                        auto* cmp = emit_cmp_eq(disc_value, disc_val, sema_ty);
                        emit_br_cond(cmp, effective_pass, fail_bb);

                        if (!ep.payload.empty())
                        {
                            set_current_block(effective_pass);

                            auto* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                            auto variant_index = static_cast<std::size_t>(ep.resolved_variant - ed->variants.data());

                            auto emit_payload_load = [&](std::size_t pi) -> std::tuple<IrValue*, IrType const*, dcc::types::TypePtr, IrValue*> {
                                auto* concrete_payload_type =
                                    (variant_index < layout->variant_count) ? layout->variants[variant_index].variant_payload_type_or_null : nullptr;

                                auto* payload_sema_type = (pi < ep.resolved_variant->payload.size() && ep.resolved_variant->payload[pi])
                                                              ? get_canonical_type(ep.resolved_variant->payload[pi])
                                                              : nullptr;

                                auto* payload_sema_canon = concrete_payload_type ? concrete_payload_type : payload_sema_type;
                                if (!payload_sema_canon)
                                    return {nullptr, nullptr, nullptr, nullptr};

                                auto* ir_payload_type = lower_type(payload_sema_canon);

                                auto* ir_byte = m_ctx.int_t(8, false);
                                auto* ir_payload_arr_ty = m_ctx.array_t(ir_byte, layout->payload_size);
                                IrValue* payload_arr = nullptr;
                                if (operand_ir_is_aggregate(value->type))
                                {
                                    auto* extract = m_ctx.extract(ir_payload_arr_ty, value, 1);
                                    auto name = ident_name();
                                    extract->name = m_name_pool.back();
                                    append_inst(extract);
                                    payload_arr = extract;
                                }
                                else
                                    payload_arr = value;

                                auto* payload_buf = m_ctx.alloca(m_ctx.pointer_to(ir_payload_arr_ty), ir_payload_arr_ty);
                                auto buf_name = ident_name();
                                payload_buf->name = m_name_pool.back();
                                append_inst(payload_buf);
                                append_inst(m_ctx.store(payload_arr, payload_buf));

                                auto* typed_ptr = m_ctx.bitcast(m_ctx.pointer_to(ir_payload_type), payload_buf);
                                auto ptr_name = ident_name();
                                typed_ptr->name = m_name_pool.back();
                                append_inst(typed_ptr);
                                auto* payload_val = m_ctx.load(ir_payload_type, typed_ptr);
                                auto load_name = ident_name();
                                payload_val->name = m_name_pool.back();
                                append_inst(payload_val);

                                return {payload_val, ir_payload_type, payload_sema_canon, payload_buf};
                            };

                            for (std::size_t pi = 0; pi < ep.payload.size(); ++pi)
                            {
                                if (!ep.payload[pi])
                                    continue;

                                if (ep.payload[pi]->kind == ast::PatternKind::Binding)
                                {
                                    auto [payload_val, ir_payload_type, payload_sema_type, payload_alloca] = emit_payload_load(pi);
                                    if (!payload_val)
                                        continue;

                                    auto const& bp = static_cast<ast::BindingPattern const&>(*ep.payload[pi]);
                                    if (bp.by_reference)
                                    {
                                        if (operand_expr)
                                        {
                                            auto* ir_byte = m_ctx.int_t(8, false);
                                            auto* l_ir_payload_arr_ty = m_ctx.array_t(ir_byte, layout->payload_size);
                                            auto* base_ptr = lower_addr_of(operand_expr);
                                            auto* gep = m_ctx.gep(m_ctx.pointer_to(l_ir_payload_arr_ty), base_ptr);
                                            gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, 1});
                                            auto gep_name = ident_name();
                                            gep->name = m_name_pool.back();
                                            append_inst(gep);
                                            auto* payload_ptr = m_ctx.bitcast(m_ctx.pointer_to(ir_payload_type), gep);
                                            auto bc_name = ident_name();
                                            payload_ptr->name = m_name_pool.back();
                                            append_inst(payload_ptr);
                                            auto* ptr_sema_ty = make_ptr_sema_type(payload_sema_type, dcc::types::Qual::None);
                                            m_named_values[bp.name] = MapEntry{payload_ptr, false, ptr_sema_ty, false};
                                        }
                                        else
                                        {
                                            auto* ptr_sema_ty = make_ptr_sema_type(payload_sema_type, dcc::types::Qual::None);
                                            m_named_values[bp.name] = MapEntry{payload_alloca, false, ptr_sema_ty, false};
                                        }
                                    }
                                    else if (operand_ir_is_aggregate(ir_payload_type))
                                    {
                                        auto* ptr_type = m_ctx.pointer_to(ir_payload_type, ir::Segment::None);
                                        auto* sub_alloca = m_ctx.alloca(ptr_type, ir_payload_type);
                                        auto sub_alloca_name = ident_name();
                                        sub_alloca->name = m_name_pool.back();
                                        append_inst(sub_alloca);
                                        append_inst(m_ctx.store(payload_val, sub_alloca));
                                        m_named_values[bp.name] = MapEntry{sub_alloca, true, payload_sema_type, false};
                                    }
                                    else
                                        m_named_values[bp.name] = MapEntry{payload_val, false, payload_sema_type, false};
                                }
                                else if (ep.payload[pi]->kind == ast::PatternKind::Wildcard)
                                    continue;
                                else
                                {
                                    auto [payload_val, ir_payload_type, payload_sema_type, payload_alloca] = emit_payload_load(pi);
                                    if (!payload_val)
                                        continue;

                                    IrBasicBlock* nested_target = pass_bb;
                                    if (pi + 1 < ep.payload.size())
                                        nested_target = create_block("match.nested.payload.pass");

                                    if (!lower_pattern_test(payload_val, payload_sema_type, *ep.payload[pi], nested_target, fail_bb, operand_expr))
                                    {
                                        emit_br(fail_bb);
                                        return false;
                                    }

                                    if (pi + 1 < ep.payload.size())
                                        set_current_block(nested_target);
                                }
                            }

                            if (has_nontrivial_payload && !current_block_terminated())
                                emit_br(pass_bb);
                        }

                        return true;
                    }

                    std::int64_t disc = static_cast<std::int64_t>(ep.resolved_variant->discriminant);

                    auto* ir_enum_ty = lower_type(sema_ty);
                    auto* disc_val = m_ctx.int_const(ir_enum_ty, disc);
                    auto* cmp = emit_cmp_eq(value, disc_val, sema_ty);
                    emit_br_cond(cmp, pass_bb, fail_bb);

                    if (!ep.payload.empty())
                        lower_panic(std::format("untagged enum payload lowering not supported for variant `{}`", ep.resolved_variant->name));

                    return true;
                }

                case ast::PatternKind::StructDestructure: {
                    auto const& sp = static_cast<ast::StructDestructurePattern const&>(pattern);
                    auto const* st = types::type_cast<types::StructType>(sema_ty);
                    if (!st)
                    {
                        emit_br(fail_bb);
                        return false;
                    }

                    auto const* struct_decl = reinterpret_cast<ast::StructDecl const*>(st->decl);
                    if (!struct_decl)
                    {
                        emit_br(fail_bb);
                        return false;
                    }

                    for (auto const& field : sp.fields)
                    {
                        dcc::types::TypePtr field_sema_ty = nullptr;
                        IrType const* ir_field_ty = nullptr;
                        for (auto const& sf : struct_decl->fields)
                        {
                            if (sf.name == field.field_name)
                            {
                                field_sema_ty = get_canonical_type(sf.type);
                                ir_field_ty = lower_type(field_sema_ty);
                                break;
                            }
                        }

                        if (!field_sema_ty || !field.pattern)
                            continue;

                        IrValue* field_val = nullptr;
                        if (operand_ir_is_aggregate(value->type))
                        {
                            auto* gep = m_ctx.gep(m_ctx.pointer_to(ir_field_ty, ir::Segment::None), value);
                            gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field.resolved_field_index});
                            auto name = ident_name();
                            gep->name = m_name_pool.back();
                            append_inst(gep);
                            auto* loaded = m_ctx.load(ir_field_ty, gep);
                            auto load_name = ident_name();
                            loaded->name = m_name_pool.back();
                            append_inst(loaded);
                            field_val = loaded;
                        }
                        else
                        {
                            auto* extract = m_ctx.extract(ir_field_ty, value, field.resolved_field_index);
                            auto name = ident_name();
                            extract->name = m_name_pool.back();
                            append_inst(extract);
                            field_val = extract;
                        }

                        if (field.pattern->kind == ast::PatternKind::Binding)
                        {
                            auto const& bp = static_cast<ast::BindingPattern const&>(*field.pattern);
                            if (bp.by_reference)
                            {
                                auto* ptr_sema_ty = make_ptr_sema_type(field_sema_ty, dcc::types::Qual::None);
                                if (operand_ir_is_aggregate(value->type))
                                {
                                    auto* gep = m_ctx.gep(m_ctx.pointer_to(ir_field_ty, ir::Segment::None), value);
                                    gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field.resolved_field_index});
                                    auto name = ident_name();
                                    gep->name = m_name_pool.back();
                                    append_inst(gep);
                                    m_named_values[bp.name] = MapEntry{gep, false, ptr_sema_ty, false};
                                }
                                else
                                {
                                    auto* ptr_type = m_ctx.pointer_to(ir_field_ty, ir::Segment::None);
                                    auto* alloca = m_ctx.alloca(ptr_type, ir_field_ty);
                                    auto name = ident_name();
                                    alloca->name = m_name_pool.back();
                                    append_inst(alloca);
                                    append_inst(m_ctx.store(field_val, alloca));
                                    m_named_values[bp.name] = MapEntry{alloca, false, ptr_sema_ty, false};
                                }
                            }
                            else if (operand_ir_is_aggregate(ir_field_ty))
                            {
                                auto* ptr_type = m_ctx.pointer_to(ir_field_ty, ir::Segment::None);
                                auto* alloca = m_ctx.alloca(ptr_type, ir_field_ty);
                                auto name = ident_name();
                                alloca->name = m_name_pool.back();
                                append_inst(alloca);
                                append_inst(m_ctx.store(field_val, alloca));
                                m_named_values[bp.name] = MapEntry{alloca, true, field_sema_ty, false};
                            }
                            else
                                m_named_values[bp.name] = MapEntry{field_val, false, field_sema_ty, false};
                        }
                        else if (field.pattern->kind == ast::PatternKind::Wildcard)
                        {
                        }
                        else
                        {
                            auto* nested_pass = create_block("match.nested.pass");
                            auto* nested_fail = create_block("match.nested.fail");
                            if (!lower_pattern_test(field_val, field_sema_ty, *field.pattern, nested_pass, nested_fail, operand_expr))
                            {
                                emit_br(fail_bb);
                                return false;
                            }

                            set_current_block(nested_pass);
                        }
                    }

                    emit_br(pass_bb);
                    return true;
                }
            }
            emit_br(fail_bb);
            return false;
        }

        static bool operand_ir_is_aggregate(IrType const* t)
        {
            if (!t)
                return false;

            return t->kind == IrTypeKind::Aggregate || t->kind == IrTypeKind::Array || t->kind == IrTypeKind::Slice;
        }

        IrValue* emit_cmp_eq(IrValue* lhs, IrValue* rhs, dcc::types::TypePtr)
        {
            auto* cmp = m_ctx.cmp_eq(lhs, rhs);
            auto name = ident_name();
            cmp->name = m_name_pool.back();
            append_inst(cmp);
            return cmp;
        }

        IrValue* emit_cmp_ge(IrValue* lhs, IrValue* rhs, dcc::types::TypePtr sema_ty)
        {
            auto cat = classify_type(sema_ty);
            IrValue* cmp = nullptr;

            if (cat == TypeCat::Float)
                cmp = m_ctx.cmp_oge(lhs, rhs);
            else if (cat == TypeCat::Unsigned)
                cmp = m_ctx.cmp_uge(lhs, rhs);
            else
                cmp = m_ctx.cmp_ge(lhs, rhs);

            auto name = ident_name();
            cmp->name = m_name_pool.back();
            append_inst(cmp);
            return cmp;
        }

        IrValue* emit_cmp_le(IrValue* lhs, IrValue* rhs, dcc::types::TypePtr sema_ty)
        {
            auto cat = classify_type(sema_ty);
            IrValue* cmp = nullptr;

            if (cat == TypeCat::Float)
                cmp = m_ctx.cmp_ole(lhs, rhs);
            else if (cat == TypeCat::Unsigned)
                cmp = m_ctx.cmp_ule(lhs, rhs);
            else
                cmp = m_ctx.cmp_le(lhs, rhs);

            auto name = ident_name();
            cmp->name = m_name_pool.back();
            append_inst(cmp);
            return cmp;
        }

        IrValue* emit_cmp_lt(IrValue* lhs, IrValue* rhs, dcc::types::TypePtr sema_ty)
        {
            auto cat = classify_type(sema_ty);
            IrValue* cmp = nullptr;

            if (cat == TypeCat::Float)
                cmp = m_ctx.cmp_olt(lhs, rhs);
            else if (cat == TypeCat::Unsigned)
                cmp = m_ctx.cmp_ult(lhs, rhs);
            else
                cmp = m_ctx.cmp_lt(lhs, rhs);

            auto name = ident_name();
            cmp->name = m_name_pool.back();
            append_inst(cmp);
            return cmp;
        }

        IrValue* lower_block_expr(ast::BlockExpr const* be)
        {
            auto* tail_val = lower_block_body(be->body);

            auto* block_type = get_sema_resolved_type(be);
            if (block_type && tail_val)
            {
                auto* tail_sema_type = be->body.tail ? get_sema_resolved_type(be->body.tail) : nullptr;

                if (auto* arrt = types::type_cast<types::ArrayType>(block_type))
                {
                    if (tail_sema_type && tail_sema_type == arrt->element)
                    {
                        auto* ir_arr_type = lower_type(block_type);
                        auto* agg = m_ctx.aggregate(ir_arr_type);
                        agg->values.push_back(tail_val);
                        auto name = ident_name();
                        agg->name = m_name_pool.back();
                        append_inst(agg);
                        return agg;
                    }
                }
                else if (auto* slicet = types::type_cast<types::SliceType>(block_type))
                {
                    if (tail_sema_type && tail_sema_type == slicet->element)
                    {
                        auto* ir_elem_type = lower_type(slicet->element);
                        auto* ir_arr_type = m_ctx.array_t(ir_elem_type, 1);
                        auto* ir_arr_ptr_type = m_ctx.pointer_to(ir_arr_type);

                        auto* alloca = m_ctx.alloca(ir_arr_ptr_type, ir_arr_type);
                        auto alloca_name = ident_name();
                        alloca->name = m_name_pool.back();
                        append_inst(alloca);

                        auto* gep_elem = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), alloca);
                        gep_elem->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(m_ctx.int_t(64, false), 0), 0});
                        auto gep_name = ident_name();
                        gep_elem->name = m_name_pool.back();
                        append_inst(gep_elem);
                        append_inst(m_ctx.store(tail_val, gep_elem));

                        auto* ptr_to_first = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), alloca);
                        ptr_to_first->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(m_ctx.int_t(64, false), 0), 0});
                        auto ptr_name = ident_name();
                        ptr_to_first->name = m_name_pool.back();
                        append_inst(ptr_to_first);

                        auto* len_val = m_ctx.int_const(m_ctx.usize_t(), 1);
                        auto* ir_slice_type = lower_type(block_type);
                        auto* slice_agg = m_ctx.aggregate(ir_slice_type);
                        slice_agg->values.push_back(ptr_to_first);
                        slice_agg->values.push_back(len_val);
                        auto slice_name = ident_name();
                        slice_agg->name = m_name_pool.back();
                        append_inst(slice_agg);
                        return slice_agg;
                    }
                }
            }

            return tail_val;
        }

        IrValue* lower_short_circuit_and(ast::BinaryExpr const* bin, dcc::types::TypePtr sema_ty, IrType const* ir_ty)
        {
            std::ignore = sema_ty;
            auto* rhs_bb = create_block("sc.rhs");
            auto* merge_bb = create_block("sc.merge");

            auto* lhs_val = lower_expr(bin->lhs);
            auto* entry_bb = m_current_block;

            emit_br_cond(lhs_val, rhs_bb, merge_bb);

            set_current_block(rhs_bb);
            auto* rhs_val = lower_expr(bin->rhs);
            emit_br(merge_bb);

            set_current_block(merge_bb);
            auto* phi = emit_phi(ir_ty);
            add_phi_incoming(phi, m_ctx.bool_const(false), entry_bb);
            add_phi_incoming(phi, rhs_val ? rhs_val : m_ctx.bool_const(false), rhs_bb);
            return phi;
        }

        IrValue* lower_short_circuit_or(ast::BinaryExpr const* bin, dcc::types::TypePtr sema_ty, IrType const* ir_ty)
        {
            std::ignore = sema_ty;
            auto* rhs_bb = create_block("sc.rhs");
            auto* merge_bb = create_block("sc.merge");

            auto* lhs_val = lower_expr(bin->lhs);
            auto* entry_bb = m_current_block;

            emit_br_cond(lhs_val, merge_bb, rhs_bb);

            set_current_block(rhs_bb);
            auto* rhs_val = lower_expr(bin->rhs);
            emit_br(merge_bb);

            set_current_block(merge_bb);
            auto* phi = emit_phi(ir_ty);
            add_phi_incoming(phi, m_ctx.bool_const(true), entry_bb);
            add_phi_incoming(phi, rhs_val ? rhs_val : m_ctx.bool_const(false), rhs_bb);
            return phi;
        }

        IrContext& m_ctx;
        sema::SpecializationRegistry const* m_spec_reg{};
        sema::ModuleGraph const* m_module_graph{};
        dcc::types::TypeContext* m_type_ctx{};
        IrModule* m_module{};
        sema::ModuleInfo const* m_entry_module{};
        std::vector<std::string_view> m_module_path;
        std::vector<std::string_view> m_module_path_cache;
        dcc::ir::mangle::NominalResolver m_nominal_resolver;

        std::unordered_map<ast::FuncDecl const*, IrFunction*> m_func_map;
        std::unordered_set<ast::FuncDecl const*> m_definition_functions;

        IrFunction* m_current_func{};
        ast::FuncDecl const* m_current_func_decl{};
        IrBasicBlock* m_current_block{};
        std::uint32_t m_next_local_id{};
        std::uint32_t m_next_block_id{1};
        std::unordered_map<ast::Decl const*, MapEntry> m_value_map;
        std::list<std::string> m_name_pool;
        std::unordered_map<std::string_view, MapEntry> m_named_values;
        std::pmr::vector<ScopeFrame> m_scope_frames;
        std::pmr::vector<LoopFrame> m_loop_stack;

        bool m_bounds_check{false};
        sm::SourceManager const* m_source_manager{};

        sm::SourceRange m_active_range{};
        std::uint32_t m_next_scope_id{};

        IrFunction* m_assert_func{};
        IrGlobalRef* m_assert_func_ref{};

        std::unordered_map<ast::VarDecl const*, IrGlobal*> m_global_map;
        std::vector<ast::VarDecl const*> m_global_order;

        void lower_globals(sema::ModuleInfo const& mod)
        {
            collect_globals(mod);

            m_string_globals.clear();
            m_u16_string_globals.clear();
            m_next_str_id = 0;

            for (auto* vd : m_global_order)
            {
                auto* ir_type = lower_type(get_canonical_type(vd->type));
                bool is_const = has_const_qual(vd->type);

                auto mangled = dcc::ir::mangle::mangle_global(m_module_path, *vd, get_canonical_type(vd->type), {}, m_nominal_resolver);
                m_name_pool.push_back(std::move(mangled));
                auto name_sv = std::string_view{m_name_pool.back()};

                auto* ir_global = m_ctx.global(name_sv, ir_type, nullptr, is_const);

                auto storage = vd->sema.storage;
                if (storage == ast::StorageClass::Static)
                {
                    ir_global->linkage = Linkage::Internal;
                }
                else if (storage == ast::StorageClass::Extern)
                {
                    ir_global->linkage = Linkage::External;
                }
                else
                {
                    ir_global->linkage = vd->is_public ? Linkage::External : Linkage::Internal;
                }

                ir_global->is_dll_import = vd->sema.is_dll_import;
                ir_global->is_dll_export = vd->sema.is_dll_export;
                ir_global->alignment = vd->sema.alignment;
                if (!vd->sema.section.empty())
                    ir_global->section = vd->sema.section;

                if (vd->init && storage != ast::StorageClass::Extern)
                {
                    auto* init_val = lower_constant_expr(vd->init, get_canonical_type(vd->type));
                    ir_global->init = init_val;
                }

                m_global_map[vd] = ir_global;
                m_module->globals.push_back(ir_global);
            }
        }

        void collect_globals(sema::ModuleInfo const& mod)
        {
            m_global_order.clear();
            std::unordered_set<ast::VarDecl const*> seen;

            auto add_from_tu = [&](sema::ModuleInfo const& m) {
                if (!m.tu)
                    return;
                for (auto* d : m.tu->decls)
                {
                    auto* vd = ast::node_cast<ast::VarDecl>(d);
                    if (!vd)
                        continue;
                    auto storage = vd->sema.storage;
                    if (storage == ast::StorageClass::ModuleGlobal || storage == ast::StorageClass::Static || storage == ast::StorageClass::Extern)
                    {
                        if (seen.insert(vd).second)
                            m_global_order.push_back(vd);
                    }
                }
            };

            add_from_tu(mod);
        }

        static bool has_const_qual(ast::TypeExpr const* type)
        {
            if (!type)
                return false;

            if (auto* qt = ast::node_cast<ast::QualifiedType>(type))
                return ast::has_qual(qt->quals, ast::Qual::Const);

            return false;
        }

        std::unordered_map<std::string, std::string> m_string_globals;
        std::unordered_map<std::string, std::string> m_u16_string_globals;
        std::uint32_t m_next_str_id{};

        IrGlobal* get_or_create_string_global(std::string_view content, bool with_null)
        {
            std::string key(content);
            if (with_null)
                key.push_back('\0');

            auto it = m_string_globals.find(key);
            if (it != m_string_globals.end())
                for (auto* g : m_module->globals)
                    if (g->name == it->second)
                        return g;

            auto name = std::format(".str.{}", m_next_str_id++);
            m_name_pool.push_back(name);
            auto name_sv = std::string_view{m_name_pool.back()};

            auto* u8_type = m_ctx.int_t(8, false);
            std::uint64_t arr_len = content.size() + (with_null ? 1 : 0);
            auto* arr_type = m_ctx.array_t(u8_type, arr_len);

            auto* agg = m_ctx.aggregate(arr_type);
            for (std::size_t i = 0; i < content.size(); ++i)
            {
                auto* byte_val = m_ctx.int_const(u8_type, static_cast<std::int64_t>(static_cast<unsigned char>(content[i])));
                agg->values.push_back(byte_val);
            }

            if (with_null)
                agg->values.push_back(m_ctx.int_const(u8_type, 0));

            auto* ir_global = m_ctx.global(name_sv, arr_type, agg, true);
            m_module->globals.push_back(ir_global);

            m_string_globals[key] = std::string(name_sv);
            return ir_global;
        }

        IrGlobal* get_or_create_u16_string_global(std::u16string_view content, bool with_null)
        {
            std::string key(reinterpret_cast<char const*>(content.data()), content.size() * sizeof(char16_t));
            if (with_null)
            {
                char16_t zero = 0;
                key.append(reinterpret_cast<char const*>(&zero), sizeof(char16_t));
            }

            auto it = m_u16_string_globals.find(key);
            if (it != m_u16_string_globals.end())
                for (auto* g : m_module->globals)
                    if (g->name == it->second)
                        return g;

            auto name = std::format(".u16str.{}", m_next_str_id++);
            m_name_pool.push_back(name);
            auto name_sv = std::string_view{m_name_pool.back()};

            auto* u16_type = m_ctx.int_t(16, false);
            std::uint64_t arr_len = content.size() + (with_null ? 1 : 0);
            auto* arr_type = m_ctx.array_t(u16_type, arr_len);

            auto* agg = m_ctx.aggregate(arr_type);
            for (std::size_t i = 0; i < content.size(); ++i)
            {
                auto* elem_val = m_ctx.int_const(u16_type, static_cast<std::int64_t>(content[i]));
                agg->values.push_back(elem_val);
            }

            if (with_null)
                agg->values.push_back(m_ctx.int_const(u16_type, 0));

            auto* ir_global = m_ctx.global(name_sv, arr_type, agg, true);
            m_module->globals.push_back(ir_global);

            m_u16_string_globals[key] = std::string(name_sv);
            return ir_global;
        }

        IrValue* lower_constant_expr(ast::Expr const* expr, dcc::types::TypePtr target_type)
        {
            if (!expr)
                lower_panic("null expression in constant context");

            if (expr->sema.const_value)
                return materialize_comptime(*expr->sema.const_value, target_type);

            switch (expr->kind)
            {
                case ast::ExprKind::IntLiteral: {
                    auto* lit = static_cast<ast::IntLiteralExpr const*>(expr);
                    auto* ir_ty = lower_type(target_type);
                    return m_ctx.int_const(ir_ty, lit->value);
                }
                case ast::ExprKind::FloatLiteral: {
                    auto* lit = static_cast<ast::FloatLiteralExpr const*>(expr);
                    auto* ir_ty = lower_type(target_type);
                    return m_ctx.float_const(ir_ty, lit->value);
                }
                case ast::ExprKind::BoolLiteral: {
                    auto* lit = static_cast<ast::BoolLiteralExpr const*>(expr);
                    return m_ctx.bool_const(lit->value);
                }
                case ast::ExprKind::CharLiteral: {
                    auto* lit = static_cast<ast::CharLiteralExpr const*>(expr);
                    return m_ctx.int_const(m_ctx.int_t(8, false), static_cast<std::int64_t>(lit->codepoint));
                }
                case ast::ExprKind::U16CharLiteral: {
                    auto* lit = static_cast<ast::U16CharLiteralExpr const*>(expr);
                    return m_ctx.int_const(m_ctx.int_t(16, false), static_cast<std::int64_t>(lit->value));
                }
                case ast::ExprKind::NullLiteral: {
                    auto* ir_ty = lower_type(target_type);
                    return m_ctx.null_const(ir_ty);
                }
                case ast::ExprKind::StringLiteral: {
                    auto* sl = static_cast<ast::StringLiteralExpr const*>(expr);
                    return lower_string_literal_value(sl, target_type);
                }
                case ast::ExprKind::U16StringLiteral: {
                    auto* sl = static_cast<ast::U16StringLiteralExpr const*>(expr);
                    return lower_u16_string_literal_value(sl, target_type);
                }
                case ast::ExprKind::StructLiteral: {
                    auto* sl = static_cast<ast::StructLiteralExpr const*>(expr);
                    return lower_struct_literal_constant(sl, target_type);
                }
                case ast::ExprKind::Ident: {
                    auto* id = static_cast<ast::IdentExpr const*>(expr);
                    auto* resolved = id->sema.resolved_decl;

                    if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                    {
                        auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                        if (global)
                        {
                            auto* ptr_type = m_ctx.pointer_to(global->type);
                            return m_ctx.global_ref(global, ptr_type);
                        }
                    }

                    if (auto* fd = ast::node_cast<ast::FuncDecl>(resolved))
                    {
                        auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(fd));
                        if (ir_func)
                            return m_ctx.func_ref(ir_func);
                    }

                    lower_panic(expr, "identifier cannot be lowered as constant");
                }
                case ast::ExprKind::PathExpr: {
                    auto* pe = static_cast<ast::PathExpr const*>(expr);
                    auto* resolved = pe->sema.resolved_decl;
                    if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                    {
                        auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                        if (global)
                        {
                            auto* ptr_type = m_ctx.pointer_to(global->type);
                            return m_ctx.global_ref(global, ptr_type);
                        }
                    }

                    if (auto* fd = ast::node_cast<ast::FuncDecl>(resolved))
                    {
                        auto* ir_func = get_or_create_func_ref(const_cast<ast::FuncDecl*>(fd));
                        if (ir_func)
                            return m_ctx.func_ref(ir_func);
                    }
                    lower_panic(expr, "path expr cannot be lowered as constant");
                }
                case ast::ExprKind::Cast: {
                    auto* c = static_cast<ast::CastExpr const*>(expr);
                    auto* operand_val = lower_constant_expr(c->operand, get_sema_resolved_type(c->operand));

                    if (auto* ic = ir_cast<IrIntConstant>(operand_val))
                    {
                        auto* dst_ir_ty = lower_type(target_type);
                        return m_ctx.int_const(dst_ir_ty, ic->value);
                    }

                    if (ir_cast<IrNullConstant>(operand_val))
                    {
                        auto* dst_ir_ty = lower_type(target_type);
                        if (dst_ir_ty->kind == IrTypeKind::Pointer)
                            return m_ctx.null_const(dst_ir_ty);
                        return operand_val;
                    }
                    lower_panic(expr, "non-scalar constant cast not supported");
                }
                default:
                    lower_panic(expr, std::format("expression kind {} cannot be lowered as constant", static_cast<int>(expr->kind)));
            }
        }

        IrValue* lower_struct_literal_constant(ast::StructLiteralExpr const* sl, dcc::types::TypePtr target_type)
        {
            if (!target_type)
                target_type = get_sema_resolved_type(sl);

            if (!target_type)
                lower_panic(sl, "struct literal without target type in constant");

            if (auto* nt = types::type_cast<types::NominalType>(target_type))
                target_type = nt->underlying;

            auto* ir_ty = lower_type(target_type);

            std::uint32_t field_count = 0;
            std::vector<types::TypePtr> field_types;

            if (auto* st = types::type_cast<types::StructType>(target_type))
            {
                auto* sd = reinterpret_cast<ast::StructDecl const*>(st->decl);
                field_count = static_cast<std::uint32_t>(sd->fields.size());
                for (auto const& f : sd->fields)
                {
                    auto* ft = get_canonical_type(f.type);
                    field_types.push_back(ft);
                }
            }
            else if (auto* ut = types::type_cast<types::UnionType>(target_type))
            {
                auto* ud = reinterpret_cast<ast::UnionDecl const*>(ut->decl);
                field_count = static_cast<std::uint32_t>(ud->fields.size());
                for (auto const& f : ud->fields)
                    field_types.push_back(get_canonical_type(f.type));
            }
            else if (auto* at = types::type_cast<types::ArrayType>(target_type))
            {
                field_count = static_cast<std::uint32_t>(at->count);
                for (std::uint32_t i = 0; i < field_count; ++i)
                    field_types.push_back(at->element);
            }
            else
                lower_panic(sl, "unsupported aggregate type in constant literal");

            auto* agg = m_ctx.aggregate(ir_ty);
            agg->values.resize(field_count, nullptr);

            for (auto const& f : sl->fields)
            {
                std::uint32_t idx = f.resolved_field_index;
                if (idx >= field_count)
                    lower_panic(sl, "field index out of range in constant struct literal");

                if (!f.value)
                    continue;

                auto* val = lower_constant_expr(f.value, (idx < field_types.size()) ? field_types[idx] : nullptr);

                agg->values[idx] = val;
            }

            for (std::uint32_t i = 0; i < field_count; ++i)
            {
                if (!agg->values[i])
                {
                    auto* ft = (i < field_types.size()) ? field_types[i] : nullptr;
                    agg->values[i] = zero_value(lower_type(ft ? ft : target_type));
                }
            }

            return agg;
        }

        IrValue* lower_string_literal_value(ast::StringLiteralExpr const* sl, dcc::types::TypePtr target_type)
        {
            auto& content = sl->value;
            if (!target_type)
                target_type = get_sema_resolved_type(sl);

            bool needs_null = false;
            bool is_slice_target = false;
            if (target_type)
            {
                if (target_type->kind == dcc::types::TypeKind::Pointer)
                    needs_null = true;
                else if (target_type->kind == dcc::types::TypeKind::Slice)
                    is_slice_target = true;
            }

            auto* str_global = get_or_create_string_global(content, needs_null);

            if (is_slice_target)
            {
                auto* slice_ir_ty = lower_type(target_type);
                auto* ptr_type = m_ctx.pointer_to(m_ctx.int_t(8, false));
                auto* ptr_val = m_ctx.global_ref(str_global, ptr_type);
                auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(content.size()));

                auto* agg = m_ctx.aggregate(slice_ir_ty);
                agg->values.push_back(ptr_val);
                agg->values.push_back(len_val);
                return agg;
            }
            else
            {
                auto* ptr_type = lower_type(target_type);
                if (!ptr_type || ptr_type->kind != IrTypeKind::Pointer)
                    ptr_type = m_ctx.pointer_to(m_ctx.int_t(8, false));

                return m_ctx.global_ref(str_global, ptr_type);
            }
        }

        IrValue* lower_u16_string_literal_value(ast::U16StringLiteralExpr const* sl, dcc::types::TypePtr target_type)
        {
            auto& content = sl->value;
            if (!target_type)
                target_type = get_sema_resolved_type(sl);

            bool needs_null = false;
            bool is_slice_target = false;
            if (target_type)
            {
                if (target_type->kind == dcc::types::TypeKind::Pointer)
                    needs_null = true;
                else if (target_type->kind == dcc::types::TypeKind::Slice)
                    is_slice_target = true;
            }

            auto* str_global = get_or_create_u16_string_global(std::u16string_view{content.data(), content.size()}, needs_null);

            if (is_slice_target)
            {
                auto* slice_ir_ty = lower_type(target_type);
                auto* u16_type = m_ctx.int_t(16, false);
                auto* ptr_type = m_ctx.pointer_to(u16_type);
                auto* ptr_val = m_ctx.global_ref(str_global, ptr_type);
                auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(content.size()));

                auto* agg = m_ctx.aggregate(slice_ir_ty);
                agg->values.push_back(ptr_val);
                agg->values.push_back(len_val);
                return agg;
            }
            else
            {
                auto* u16_type = m_ctx.int_t(16, false);
                auto* ptr_type = m_ctx.pointer_to(u16_type);
                return m_ctx.global_ref(str_global, ptr_type);
            }
        }

        IrValue* materialize_comptime(dcc::comptime::Value const& cv, dcc::types::TypePtr target_type)
        {
            switch (cv.kind())
            {
                case dcc::comptime::Value::Kind::Int:
                    return m_ctx.int_const(lower_type(cv.type), cv.get_int());
                case dcc::comptime::Value::Kind::Float:
                    return m_ctx.float_const(lower_type(cv.type), cv.get_float());
                case dcc::comptime::Value::Kind::Bool:
                    return m_ctx.bool_const(cv.get_bool());
                case dcc::comptime::Value::Kind::Char:
                    return m_ctx.int_const(m_ctx.int_t(8, false), static_cast<std::int64_t>(cv.get_char()));
                case dcc::comptime::Value::Kind::Null:
                    return m_ctx.null_const(lower_type(cv.type));
                case dcc::comptime::Value::Kind::String: {
                    auto& s = cv.get_string();
                    if (!target_type)
                        target_type = cv.type;

                    bool is_u16 = false;
                    if (target_type)
                    {
                        auto const* pointee = [&]() -> dcc::types::TypePtr {
                            if (auto* pt = types::type_cast<types::PointerType>(target_type))
                                return pt->pointee;
                            if (auto* st = types::type_cast<types::SliceType>(target_type))
                                return st->element;
                            return nullptr;
                        }();
                        if (pointee)
                            if (auto* it = types::type_cast<types::IntType>(pointee))
                                if (it->bits == 16 && !it->is_signed)
                                    is_u16 = true;
                    }

                    if (is_u16)
                    {
                        auto u16v = std::u16string_view{reinterpret_cast<char16_t const*>(s.data()), s.size() / sizeof(char16_t)};
                        bool needs_null = target_type && target_type->kind == dcc::types::TypeKind::Pointer;
                        auto* str_global = get_or_create_u16_string_global(u16v, needs_null);
                        auto* u16_ir = m_ctx.int_t(16, false);

                        if (target_type && target_type->kind == dcc::types::TypeKind::Slice)
                        {
                            auto* slice_ir_ty = lower_type(target_type);
                            auto* ptr_type = m_ctx.pointer_to(u16_ir);
                            auto* ptr_val = m_ctx.global_ref(str_global, ptr_type);
                            auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(u16v.size()));
                            auto* agg = m_ctx.aggregate(slice_ir_ty);
                            agg->values.push_back(ptr_val);
                            agg->values.push_back(len_val);
                            return agg;
                        }
                        else
                        {
                            auto* ptr_type = m_ctx.pointer_to(u16_ir);
                            return m_ctx.global_ref(str_global, ptr_type);
                        }
                    }
                    else
                    {
                        bool needs_null = target_type && target_type->kind == dcc::types::TypeKind::Pointer;
                        auto* str_global = get_or_create_string_global(s, needs_null);

                        if (target_type && target_type->kind == dcc::types::TypeKind::Slice)
                        {
                            auto* slice_ir_ty = lower_type(target_type);
                            auto* ptr_type = m_ctx.pointer_to(m_ctx.int_t(8, false));
                            auto* ptr_val = m_ctx.global_ref(str_global, ptr_type);
                            auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(s.size()));
                            auto* agg = m_ctx.aggregate(slice_ir_ty);
                            agg->values.push_back(ptr_val);
                            agg->values.push_back(len_val);
                            return agg;
                        }
                        else
                        {
                            auto* ptr_type = m_ctx.pointer_to(m_ctx.int_t(8, false));
                            return m_ctx.global_ref(str_global, ptr_type);
                        }
                    }
                }
                case dcc::comptime::Value::Kind::Aggregate: {
                    auto type_to_check = target_type ? target_type : cv.type;

                    if (auto* et = types::type_cast<types::EnumType>(type_to_check))
                    {
                        if (et->is_tagged && et->tagged_layout)
                        {
                            auto* layout = et->tagged_layout;
                            auto* ir_ty = lower_type(type_to_check);
                            auto* agg = m_ctx.aggregate(ir_ty);

                            if (cv.size() > 0)
                            {
                                auto* disc_val = materialize_comptime(cv.at(0), nullptr);
                                agg->values.push_back(disc_val);
                            }

                            if (layout->payload_size > 0)
                            {
                                if (cv.size() > 1)
                                {
                                    auto* payload_val = materialize_comptime(cv.at(1), nullptr);
                                    agg->values.push_back(payload_val);
                                }
                                else
                                {
                                    auto* ir_byte = m_ctx.int_t(8, false);
                                    auto* ir_payload_arr_ty = m_ctx.array_t(ir_byte, layout->payload_size);
                                    auto* zero_payload = zero_value(ir_payload_arr_ty);
                                    agg->values.push_back(zero_payload);
                                }
                            }

                            return agg;
                        }
                    }

                    auto* ir_ty = lower_type(type_to_check);
                    auto* agg = m_ctx.aggregate(ir_ty);
                    for (std::size_t i = 0; i < cv.size(); ++i)
                    {
                        auto& elem = cv.at(i);
                        auto* elem_val = materialize_comptime(elem, nullptr);
                        agg->values.push_back(elem_val);
                    }

                    return agg;
                }
                case dcc::comptime::Value::Kind::Slice: {
                    auto* ir_ty = lower_type(target_type ? target_type : cv.type);

                    std::string content;
                    auto* slice_ty = types::type_cast<types::SliceType>(cv.type ? cv.type : target_type);
                    bool is_string_slice = slice_ty && slice_ty->element && slice_ty->element->kind == types::TypeKind::Char;
                    if (is_string_slice)
                    {
                        for (std::size_t i = 0; i < cv.size(); ++i)
                        {
                            auto& elem = cv.at(i);
                            if (elem.kind() == dcc::comptime::Value::Kind::Char)
                                content.push_back(static_cast<char>(elem.get_char()));
                            else if (elem.kind() == dcc::comptime::Value::Kind::Int)
                                content.push_back(static_cast<char>(elem.get_int()));
                        }
                    }

                    if (is_string_slice && !content.empty())
                    {
                        auto* str_global = get_or_create_string_global(content, false);
                        auto* ptr_type = m_ctx.pointer_to(m_ctx.int_t(8, false));
                        auto* ptr_val = m_ctx.global_ref(str_global, ptr_type);
                        auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(content.size()));
                        auto* agg = m_ctx.aggregate(ir_ty);
                        agg->values.push_back(ptr_val);
                        agg->values.push_back(len_val);
                        return agg;
                    }

                    lower_panic("non-string slice comptime materialization not supported");
                }
                case dcc::comptime::Value::Kind::Pointer: {
                    if (cv.is_null_ptr())
                        return m_ctx.null_const(lower_type(target_type ? target_type : cv.type));
                    lower_panic("non-null comptime pointer materialization not supported");
                }
            }
            lower_panic("unknown comptime value kind");
        }

        IrValue* zero_value(IrType const* ir_type)
        {
            if (!ir_type)
                return m_ctx.int_const(m_ctx.int_t(8, false), 0);

            switch (ir_type->kind)
            {
                case IrTypeKind::Void:
                    return nullptr;
                case IrTypeKind::Bool:
                    return m_ctx.bool_const(false);
                case IrTypeKind::Int:
                    return m_ctx.int_const(ir_type, 0);
                case IrTypeKind::Float:
                    return m_ctx.float_const(ir_type, 0.0);
                case IrTypeKind::Pointer:
                    return m_ctx.null_const(ir_type);
                case IrTypeKind::Func:
                    return m_ctx.null_const(m_ctx.pointer_to(ir_type));
                case IrTypeKind::Array: {
                    auto* arr = static_cast<IrArrayType const*>(ir_type);
                    auto* agg = m_ctx.aggregate(ir_type);
                    agg->values.resize(static_cast<std::size_t>(arr->count), zero_value(arr->element));
                    return agg;
                }
                case IrTypeKind::Aggregate: {
                    auto* at = static_cast<IrAggregateType const*>(ir_type);
                    auto* agg = m_ctx.aggregate(ir_type);
                    for (auto* member : at->members)
                        agg->values.push_back(zero_value(member));
                    return agg;
                }
                case IrTypeKind::Slice: {
                    auto* ptr_type = m_ctx.pointer_to(m_ctx.int_t(8, false));
                    auto* null_ptr = m_ctx.null_const(ptr_type);
                    auto* zero_len = m_ctx.int_const(m_ctx.usize_t(), 0);
                    auto* agg = m_ctx.aggregate(ir_type);
                    agg->values.push_back(null_ptr);
                    agg->values.push_back(zero_len);
                    return agg;
                }
            }
            return m_ctx.int_const(m_ctx.int_t(8, false), 0);
        }

        std::unordered_map<types::Type const*, IrType const*> m_struct_type_cache;

        [[nodiscard]] std::unordered_map<void const*, types::TypePtr> build_template_subst(std::span<ast::TemplateParam const> template_params,
                                                                                           std::span<types::TypePtr const> template_args) const
        {
            std::unordered_map<void const*, types::TypePtr> subst;
            auto n = std::min(template_params.size(), template_args.size());
            for (std::size_t i = 0; i < n; ++i)
                subst[static_cast<void const*>(&template_params[i])] = template_args[i];

            return subst;
        }

        [[nodiscard]] std::unordered_map<void const*, types::TypePtr> build_subst_from_user_type(types::TypePtr type) const
        {
            if (!type)
                return {};

            if (type->kind == types::TypeKind::Struct)
            {
                auto* st = types::type_cast<types::StructType>(type);
                if (!st || st->template_args.empty())
                    return {};

                auto* sd = reinterpret_cast<ast::StructDecl const*>(st->decl);
                return build_template_subst(sd->template_params, st->template_args);
            }
            if (type->kind == types::TypeKind::Union)
                return {};

            if (type->kind == types::TypeKind::Enum)
            {
                auto* et = types::type_cast<types::EnumType>(type);
                if (!et || et->template_args.empty())
                    return {};

                auto* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                return build_template_subst(ed->template_params, et->template_args);
            }

            return {};
        }

        [[nodiscard]] types::TypePtr get_field_canonical_with_subst(types::TypePtr parent_type, ast::FieldDecl const& field,
                                                                    std::unordered_map<void const*, types::TypePtr> const* inherited_subst = nullptr) const
        {
            auto* ft = get_canonical_type(field.type);
            if (inherited_subst && !inherited_subst->empty())
                ft = substitute_type(ft, *inherited_subst);

            if (!inherited_subst || inherited_subst->empty())
            {
                auto subst = build_subst_from_user_type(parent_type);
                if (!subst.empty())
                    ft = substitute_type(ft, subst);
            }

            return ft;
        }

        [[nodiscard]] types::TypePtr substitute_type(types::TypePtr type, std::unordered_map<void const*, types::TypePtr> const& subst) const
        {
            if (!type)
                return type;

            if (auto* tpt = types::type_cast<types::TemplateParamType>(type))
            {
                auto it = subst.find(tpt->param);
                if (it != subst.end())
                    return it->second;
                return type;
            }

            if (type->kind == types::TypeKind::Struct || type->kind == types::TypeKind::Union || type->kind == types::TypeKind::Enum)
            {
                auto* ut = static_cast<types::UserType const*>(type);
                bool changed = false;
                std::vector<types::TypePtr> new_args;
                new_args.reserve(ut->template_args.size());
                for (auto* ta : ut->template_args)
                {
                    auto* sub = substitute_type(ta, subst);
                    new_args.push_back(sub);
                    if (sub != ta)
                        changed = true;
                }
                if (changed && m_type_ctx)
                    return m_type_ctx->nominal_t(type->kind, const_cast<void*>(static_cast<void const*>(ut->decl)), new_args);

                return type;
            }

            if (auto* pt = types::type_cast<types::PointerType>(type))
            {
                auto* sub = substitute_type(pt->pointee, subst);
                if (sub != pt->pointee && m_type_ctx)
                    return m_type_ctx->pointer_to(sub, pt->pointee_quals);
                return type;
            }

            if (auto* at = types::type_cast<types::ArrayType>(type))
            {
                auto* sub = substitute_type(at->element, subst);
                if (sub != at->element && m_type_ctx)
                    return m_type_ctx->array_t(sub, at->count);
                return type;
            }

            if (auto* st = types::type_cast<types::SliceType>(type))
            {
                auto* sub = substitute_type(st->element, subst);
                if (sub != st->element && m_type_ctx)
                    return m_type_ctx->slice_t(sub, st->element_quals);
                return type;
            }

            if (auto* ft = types::type_cast<types::FamType>(type))
            {
                auto* sub = substitute_type(ft->element, subst);
                if (sub != ft->element && m_type_ctx)
                    return m_type_ctx->fam_t(sub);
                return type;
            }

            if (auto* fpt = types::type_cast<types::FuncPtrType>(type))
            {
                bool changed = false;
                auto* sub_ret = substitute_type(fpt->return_type, subst);
                if (sub_ret != fpt->return_type)
                    changed = true;

                std::vector<types::TypePtr> sub_params;
                sub_params.reserve(fpt->params.size());
                for (auto* p : fpt->params)
                {
                    auto* sp = substitute_type(p, subst);
                    sub_params.push_back(sp);
                    if (sp != p)
                        changed = true;
                }
                if (changed && m_type_ctx)
                    return m_type_ctx->funcptr_t(sub_ret, sub_params);

                return type;
            }

            return type;
        }

        IrType const* lower_user_type(dcc::types::TypePtr type)
        {
            if (!type)
                return m_ctx.void_t();

            {
                auto it = m_struct_type_cache.find(type);
                if (it != m_struct_type_cache.end())
                    return it->second;
            }

            if (auto* st = types::type_cast<types::StructType>(type))
            {
                auto* sd = reinterpret_cast<ast::StructDecl const*>(st->decl);

                auto* placeholder = m_ctx.make<IrAggregateType>();
                placeholder->byte_size = type->byte_size;
                placeholder->byte_align = type->byte_align;
                m_struct_type_cache[type] = placeholder;

                auto subst = st->template_args.empty() ? std::unordered_map<void const*, types::TypePtr>{}
                                                       : build_template_subst(sd->template_params, st->template_args);

                std::vector<IrType const*> members;
                std::vector<types::TypePtr> subst_field_types;
                members.reserve(sd->fields.size());
                subst_field_types.reserve(sd->fields.size());

                for (auto const& f : sd->fields)
                {
                    auto* ft = get_canonical_type(f.type);

                    if (!subst.empty())
                        ft = substitute_type(ft, subst);

                    auto* ir_ft = lower_type(ft);
                    members.push_back(ir_ft);
                    subst_field_types.push_back(ft);

                    if (types::is_fam_type(ft))
                    {
                        placeholder->has_trailing_fam = true;
                        placeholder->fam_member_index = static_cast<std::uint32_t>(members.size() - 1);
                    }
                }

                std::vector<std::uint64_t> offsets;
                if (!subst.empty())
                    offsets = compute_field_offsets(subst_field_types);
                else
                    for (auto const& f : sd->fields)
                        offsets.push_back(f.byte_offset);

                placeholder->members.assign(members.begin(), members.end());
                placeholder->member_offsets.assign(offsets.begin(), offsets.end());

                return placeholder;
            }

            if (auto* ut = types::type_cast<types::UnionType>(type))
            {
                auto* ud = reinterpret_cast<ast::UnionDecl const*>(ut->decl);

                auto* placeholder = m_ctx.make<IrAggregateType>();
                placeholder->byte_size = type->byte_size;
                placeholder->byte_align = type->byte_align;
                m_struct_type_cache[type] = placeholder;

                std::vector<IrType const*> members;
                std::vector<std::uint64_t> offsets;
                members.reserve(ud->fields.size());
                offsets.reserve(ud->fields.size());

                for (auto const& f : ud->fields)
                {
                    auto* ft = get_canonical_type(f.type);
                    auto* ir_ft = lower_type(ft);
                    members.push_back(ir_ft);

                    offsets.push_back(0);
                }

                placeholder->members.assign(members.begin(), members.end());
                placeholder->member_offsets.assign(offsets.begin(), offsets.end());

                return placeholder;
            }

            if (auto* et = types::type_cast<types::EnumType>(type))
            {
                if (et->backing)
                    return lower_type(et->backing);

                if (et->is_tagged)
                {
                    auto* layout = et->tagged_layout;
                    if (!layout)
                        lower_panic("tagged enum missing layout");

                    std::vector<IrType const*> members;
                    std::vector<std::uint64_t> offsets;

                    auto* ir_disc = m_ctx.int_t(static_cast<std::uint8_t>(layout->discriminant_type->bits), layout->discriminant_type->is_signed);
                    members.push_back(ir_disc);
                    offsets.push_back(layout->discriminant_offset);

                    if (layout->payload_size > 0)
                    {
                        auto* ir_byte = m_ctx.int_t(8, false);
                        auto* ir_payload = m_ctx.array_t(ir_byte, layout->payload_size);
                        members.push_back(ir_payload);
                        offsets.push_back(layout->payload_offset);
                    }

                    auto* ir_agg = m_ctx.aggregate_t(members, offsets, layout->total_size, layout->total_align, false);

                    m_struct_type_cache[type] = ir_agg;

                    return ir_agg;
                }

                if (!et->template_args.empty())
                {
                    auto const* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                    if (ed && ed->is_tagged)
                        lower_panic(std::format("templated tagged enum `{}` reached lowering without completed layout", ed->name));
                }

                return m_ctx.int_t(32, true);
            }

            if (auto* at = types::type_cast<types::ArrayType>(type))
            {
                auto* ir_el = lower_type(at->element);
                return m_ctx.array_t(ir_el, at->count);
            }

            return lower_type(type);
        }

        IrValue* lower_struct_literal_expr(ast::StructLiteralExpr const* sl)
        {
            auto* target_type = get_sema_resolved_type(sl);
            if (!target_type)
                lower_panic(sl, "struct literal without resolved type");

            auto* ir_ty = lower_type(target_type);

            if (auto* nt = types::type_cast<types::NominalType>(target_type))
                target_type = nt->underlying;

            auto* st = types::type_cast<types::StructType>(target_type);
            auto* ut = types::type_cast<types::UnionType>(target_type);
            auto* arrt = types::type_cast<types::ArrayType>(target_type);
            auto* slicet = types::type_cast<types::SliceType>(target_type);

            auto lower_field_value = [&](ast::Expr const* value_expr) -> IrValue* {
                if (!value_expr)
                    return nullptr;

                return lower_implicit_enum_construction(value_expr, [&]() { return lower_expr(value_expr); });
            };

            if (slicet)
            {
                std::uint32_t field_count = static_cast<std::uint32_t>(sl->fields.size());
                auto* elem_sema_type = slicet->element;
                auto* ir_elem_type = lower_type(elem_sema_type);
                auto* ir_arr_type = m_ctx.array_t(ir_elem_type, field_count);
                auto* ir_arr_ptr_type = m_ctx.pointer_to(ir_arr_type);

                auto* alloca = m_ctx.alloca(ir_arr_ptr_type, ir_arr_type);
                auto alloca_name = ident_name();
                alloca->name = m_name_pool.back();
                append_inst(alloca);

                for (auto const& f : sl->fields)
                {
                    std::uint32_t idx = f.resolved_field_index;
                    if (idx >= field_count)
                        lower_panic(sl, "field index out of range");

                    if (!f.value)
                        continue;

                    auto* elem_val = lower_field_value(f.value);
                    if (!elem_val)
                        continue;

                    auto* gep = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), alloca);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(m_ctx.int_t(64, false), static_cast<std::int64_t>(idx)), 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    append_inst(m_ctx.store(elem_val, gep));
                }

                auto* ptr_to_first = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), alloca);
                ptr_to_first->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(m_ctx.int_t(64, false), 0), 0});
                auto ptr_name = ident_name();
                ptr_to_first->name = m_name_pool.back();
                append_inst(ptr_to_first);

                auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(field_count));

                auto* slice_agg = m_ctx.aggregate(ir_ty);
                slice_agg->values.push_back(ptr_to_first);
                slice_agg->values.push_back(len_val);
                auto slice_name = ident_name();
                slice_agg->name = m_name_pool.back();
                append_inst(slice_agg);
                return slice_agg;
            }

            auto* agg = m_ctx.aggregate(ir_ty);

            std::uint32_t field_count = 0;
            std::vector<dcc::types::TypePtr> field_types;

            if (st)
            {
                auto* sd = reinterpret_cast<ast::StructDecl const*>(st->decl);
                field_count = static_cast<std::uint32_t>(sd->fields.size());
                auto subst = build_subst_from_user_type(target_type);
                for (auto const& f : sd->fields)
                    field_types.push_back(get_field_canonical_with_subst(target_type, f, &subst));
            }
            else if (ut)
            {
                auto* ud = reinterpret_cast<ast::UnionDecl const*>(ut->decl);
                field_count = static_cast<std::uint32_t>(ud->fields.size());
                auto subst = build_subst_from_user_type(target_type);
                for (auto const& f : ud->fields)
                    field_types.push_back(get_field_canonical_with_subst(target_type, f, &subst));
            }
            else if (arrt)
            {
                field_count = static_cast<std::uint32_t>(arrt->count);
                for (std::uint32_t i = 0; i < field_count; ++i)
                    field_types.push_back(arrt->element);
            }
            else
                lower_panic(sl, "struct literal on non-aggregate type");

            agg->values.resize(field_count, nullptr);

            for (auto const& f : sl->fields)
            {
                std::uint32_t idx = f.resolved_field_index;
                if (idx >= field_count)
                    lower_panic(sl, "field index out of range");

                if (!f.value)
                    continue;

                auto* val = lower_field_value(f.value);
                agg->values[idx] = val;
            }

            for (std::uint32_t i = 0; i < field_count; ++i)
            {
                if (!agg->values[i])
                {
                    auto* ir_field_ty = (i < field_types.size()) ? lower_type(field_types[i]) : nullptr;
                    agg->values[i] = zero_value(ir_field_ty ? ir_field_ty : ir_ty);
                }
            }

            auto name = ident_name();
            agg->name = m_name_pool.back();
            append_inst(agg);
            return agg;
        }

        IrValue* lower_implicit_enum_construction(ast::Expr const* expr, std::function<IrValue*()> fallback_lower)
        {
            if (expr->sema.construction_kind == ast::ExprSema::ConstructionKind::Enum && expr->sema.constructed_variant)
            {
                auto* enum_type = get_sema_resolved_type(expr);
                if (!enum_type)
                    return fallback_lower();

                auto* et = types::type_cast<types::EnumType>(enum_type);
                if (!et || !et->is_tagged)
                    return fallback_lower();

                auto* variant = expr->sema.constructed_variant;
                auto* ir_enum_ty = lower_type(enum_type);

                if (ast::node_cast<ast::CallExpr>(expr))
                {
                    auto* fallback_val = fallback_lower();
                    if (fallback_val && fallback_val->type == ir_enum_ty && fallback_val->kind == IrNodeKind::Aggregate)
                        return fallback_val;
                }

                bool needs_payload_lowering =
                    ast::node_cast<ast::StringLiteralExpr>(expr) || ast::node_cast<ast::U16StringLiteralExpr>(expr) || expr->sema.const_value;

                if (!needs_payload_lowering)
                {
                    auto* fallback_val = fallback_lower();
                    if (fallback_val && fallback_val->type == ir_enum_ty && fallback_val->kind == IrNodeKind::Aggregate)
                        return fallback_val;
                }

                std::vector<IrValue*> payload_args;

                for (std::size_t pi = 0; pi < variant->payload.size(); ++pi)
                {
                    auto* payload_canon = get_canonical_type(variant->payload[pi]);
                    if (!payload_canon)
                    {
                        if (!needs_payload_lowering)
                            payload_args.push_back(fallback_lower());
                        else
                            payload_args.push_back(nullptr);

                        continue;
                    }

                    if (et->tagged_layout)
                    {
                        auto const* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                        if (ed)
                        {
                            auto variant_index = static_cast<std::size_t>(variant - ed->variants.data());
                            if (variant_index < et->tagged_layout->variant_count)
                            {
                                auto* concrete_pt = et->tagged_layout->variants[variant_index].variant_payload_type_or_null;
                                if (concrete_pt)
                                    payload_canon = concrete_pt;
                            }
                        }
                    }

                    IrValue* payload_val = nullptr;

                    if (auto* sl = ast::node_cast<ast::StringLiteralExpr>(expr))
                        payload_val = lower_string_literal_value(sl, payload_canon);
                    else if (auto* u16sl = ast::node_cast<ast::U16StringLiteralExpr>(expr))
                        payload_val = lower_u16_string_literal_value(u16sl, payload_canon);
                    else if (expr->sema.const_value)
                        payload_val = materialize_comptime(*expr->sema.const_value, payload_canon);

                    if (!payload_val && !needs_payload_lowering)
                        payload_val = fallback_lower();

                    if (payload_val)
                    {
                        auto* payload_ir_type = lower_type(payload_canon);
                        if (payload_val->type && payload_ir_type && payload_val->type != payload_ir_type)
                            payload_val = coerce_value(payload_val, payload_ir_type);
                    }

                    payload_args.push_back(payload_val);
                }

                return lower_tagged_enum_construction(variant, enum_type, payload_args);
            }

            return fallback_lower();
        }

        IrValue* coerce_value(IrValue* val, IrType const* target_ir_type)
        {
            if (!val || !target_ir_type || val->type == target_ir_type)
                return val;

            if (val->type->kind == IrTypeKind::Pointer && target_ir_type->kind == IrTypeKind::Pointer)
            {
                auto* bc = m_ctx.bitcast(target_ir_type, val);
                auto name = ident_name();
                bc->name = m_name_pool.back();
                append_inst(bc);
                return bc;
            }

            if (target_ir_type->kind == IrTypeKind::Aggregate)
            {
                auto* bc = m_ctx.bitcast(target_ir_type, val);
                auto name = ident_name();
                bc->name = m_name_pool.back();
                append_inst(bc);
                return bc;
            }

            return val;
        }

        IrValue* lower_tagged_enum_construction(ast::EnumVariant const* variant, dcc::types::TypePtr enum_type, std::span<IrValue*> payload_args)
        {
            auto* et = types::type_cast<types::EnumType>(enum_type);
            if (!et || !et->tagged_layout)
                lower_panic("tagged enum construction without layout");

            auto* layout = et->tagged_layout;
            auto* ir_ty = lower_type(enum_type);

            auto* ir_disc_type = m_ctx.int_t(static_cast<std::uint8_t>(layout->discriminant_type->bits), layout->discriminant_type->is_signed);

            auto* disc_val = m_ctx.int_const(ir_disc_type, static_cast<std::int64_t>(variant->discriminant));

            auto* agg = m_ctx.aggregate(ir_ty);
            agg->values.push_back(disc_val);

            if (layout->payload_size > 0 && !payload_args.empty() && variant->payload.size() == 1)
            {
                auto* ed = reinterpret_cast<ast::EnumDecl const*>(et->decl);
                auto variant_index = static_cast<std::size_t>(variant - ed->variants.data());
                auto* concrete_payload_type = (variant_index < layout->variant_count) ? layout->variants[variant_index].variant_payload_type_or_null : nullptr;

                auto* payload_sema_type = variant->payload[0] ? reinterpret_cast<dcc::types::TypePtr>(variant->payload[0]->sema.canonical) : nullptr;
                if (!payload_sema_type)
                    lower_panic("tagged enum payload type not resolved");

                auto* payload_sema_canon = concrete_payload_type ? concrete_payload_type : get_canonical_type(variant->payload[0]);
                if (!payload_sema_canon)
                    payload_sema_canon = payload_sema_type;

                auto* ir_byte = m_ctx.int_t(8, false);
                auto* ir_payload_arr_ty = m_ctx.array_t(ir_byte, layout->payload_size);
                auto* ir_payload_ptr_ty = m_ctx.pointer_to(ir_payload_arr_ty);

                auto* ir_payload_sema_ty = lower_type(payload_sema_canon);

                auto* payload_buf = m_ctx.alloca(ir_payload_ptr_ty, ir_payload_arr_ty);
                auto buf_name = ident_name();
                payload_buf->name = m_name_pool.back();
                append_inst(payload_buf);

                auto* zero_arr = zero_value(ir_payload_arr_ty);
                append_inst(m_ctx.store(zero_arr, payload_buf));

                auto* typed_ptr = m_ctx.bitcast(m_ctx.pointer_to(ir_payload_sema_ty), payload_buf);
                auto ptr_name = ident_name();
                typed_ptr->name = m_name_pool.back();
                append_inst(typed_ptr);
                append_inst(m_ctx.store(payload_args[0], typed_ptr));

                auto* payload_bytes = m_ctx.load(ir_payload_arr_ty, payload_buf);
                auto load_name = ident_name();
                payload_bytes->name = m_name_pool.back();
                append_inst(payload_bytes);

                agg->values.push_back(payload_bytes);
            }
            else if (layout->payload_size > 0)
            {
                auto* ir_byte = m_ctx.int_t(8, false);
                auto* ir_payload_arr_ty = m_ctx.array_t(ir_byte, layout->payload_size);
                auto* zero_init = zero_value(ir_payload_arr_ty);
                agg->values.push_back(zero_init);
            }

            auto name = ident_name();
            agg->name = m_name_pool.back();
            append_inst(agg);
            return agg;
        }

        IrValue* coerce_array_to_slice(IrValue* arr_val, dcc::types::TypePtr arr_sema_type, dcc::types::TypePtr slice_sema_type)
        {
            if (!arr_sema_type || arr_sema_type->kind != types::TypeKind::Array)
                return arr_val;

            if (!slice_sema_type || slice_sema_type->kind != types::TypeKind::Slice)
                return arr_val;

            auto const* at = types::type_cast<types::ArrayType>(arr_sema_type);
            if (!at)
                return arr_val;

            auto* ir_slice_type = lower_type(slice_sema_type);

            IrValue* ptr_val = nullptr;
            if (arr_val->type && arr_val->type->kind == IrTypeKind::Array)
            {
                auto* arr_ptr_type = m_ctx.pointer_to(arr_val->type);
                auto* temp = m_ctx.alloca(arr_ptr_type, arr_val->type);
                auto temp_name = ident_name();
                temp->name = m_name_pool.back();
                append_inst(temp);
                append_inst(m_ctx.store(arr_val, temp));

                auto* ir_elem_type = lower_type(at->element);
                auto* gep = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), temp);
                gep->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(m_ctx.int_t(64, false), 0), 0});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);
                ptr_val = gep;
            }
            else if (arr_val->type && arr_val->type->kind == IrTypeKind::Pointer)
                ptr_val = arr_val;
            else
                lower_panic("cannot coerce array to slice: unsupported IR type");

            auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(at->count));

            auto* agg = m_ctx.aggregate(ir_slice_type);
            agg->values.push_back(ptr_val);
            agg->values.push_back(len_val);
            auto agg_name = ident_name();
            agg->name = m_name_pool.back();
            append_inst(agg);
            return agg;
        }

        [[nodiscard]] static std::vector<std::uint64_t> compute_field_offsets(std::span<types::TypePtr const> field_types)
        {
            std::vector<std::uint64_t> offsets;
            offsets.reserve(field_types.size());
            std::uint64_t size = 0;
            std::uint32_t align = 1;
            for (auto* ft : field_types)
            {
                if (!ft)
                {
                    offsets.push_back(size);
                    continue;
                }

                auto field_align = ft->byte_align;
                if (field_align > align)
                    align = field_align;

                size = (size + field_align - 1) & ~(field_align - 1);
                offsets.push_back(size);
                size += ft->byte_size;
            }
            return offsets;
        }

        [[nodiscard]] types::TypePtr get_field_type_with_subst(ast::FieldAccessExpr const* fa, ast::FieldDecl const* field_decl) const
        {
            if (!field_decl)
                return nullptr;

            auto* obj_type = get_sema_resolved_type(fa->object);
            if (!obj_type)
                return get_canonical_type(field_decl->type);

            auto* pointee = obj_type;
            if (auto* pt = types::type_cast<types::PointerType>(obj_type))
                pointee = pt->pointee;

            if (auto* nt = types::type_cast<types::NominalType>(pointee))
                pointee = nt->underlying;

            auto subst = build_subst_from_user_type(pointee);
            return get_field_canonical_with_subst(pointee, *field_decl, &subst);
        }

        IrValue* lower_field_access_expr(ast::FieldAccessExpr const* fa)
        {
            auto* resolved_type = get_sema_resolved_type(fa);
            auto* ir_resolved_type = lower_type(resolved_type);

            auto* obj_sema_type = get_sema_resolved_type(fa->object);
            if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Slice)
            {
                std::uint32_t field_idx = (fa->field == "ptr") ? 0 : (fa->field == "len") ? 1 : std::numeric_limits<std::uint32_t>::max();
                if (field_idx > 1)
                    lower_panic(fa, std::format("unknown slice field: {}", fa->field));

                auto* resolved_obj = (fa->object->kind == ast::ExprKind::Ident) ? static_cast<ast::IdentExpr const*>(fa->object)->sema.resolved_decl : nullptr;

                if (resolved_obj)
                {
                    auto it = m_value_map.find(resolved_obj);
                    if (it != m_value_map.end() && it->second.is_storage)
                    {
                        auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* gep = m_ctx.gep(elem_ptr_type, it->second.value);
                        gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);

                        auto* loaded = m_ctx.load(ir_resolved_type, gep);
                        auto load_name = ident_name();
                        loaded->name = m_name_pool.back();
                        append_inst(loaded);
                        return loaded;
                    }

                    if (auto* vd = ast::node_cast<ast::VarDecl>(resolved_obj))
                    {
                        auto* global = get_or_create_global_ref(vd);
                        if (global)
                        {
                            auto* global_ptr = m_ctx.global_ref(global, m_ctx.pointer_to(global->type));
                            auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                            auto* gep = m_ctx.gep(elem_ptr_type, global_ptr);
                            gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                            auto gep_name = ident_name();
                            gep->name = m_name_pool.back();
                            append_inst(gep);

                            auto* loaded = m_ctx.load(ir_resolved_type, gep);
                            auto load_name = ident_name();
                            loaded->name = m_name_pool.back();
                            append_inst(loaded);
                            return loaded;
                        }
                    }
                }

                if (fa->object->sema.const_value)
                {
                    auto* obj_val = materialize_comptime(*fa->object->sema.const_value, obj_sema_type);
                    if (auto* agg = ir_cast<IrAggregateInst>(obj_val); agg && field_idx < agg->values.size())
                        return agg->values[field_idx];
                }

                auto* obj_val = lower_expr(fa->object);

                if (obj_val->type && obj_val->type->kind == IrTypeKind::Slice)
                {
                    auto* extracted = m_ctx.extract(ir_resolved_type, obj_val, field_idx);
                    auto name = ident_name();
                    extracted->name = m_name_pool.back();
                    append_inst(extracted);
                    return extracted;
                }

                lower_panic(fa, "cannot lower slice field access");
            }

            auto* resolved_obj = (fa->object->kind == ast::ExprKind::Ident) ? static_cast<ast::IdentExpr const*>(fa->object)->sema.resolved_decl : nullptr;

            if (resolved_obj)
            {
                auto it = m_value_map.find(resolved_obj);
                if (it != m_value_map.end() && it->second.is_storage)
                {
                    auto* field_decl = find_decl_field(fa);
                    if (!field_decl)
                        lower_panic(fa, "cannot find field decl");

                    std::uint32_t field_idx = field_decl->index;
                    auto* field_sema_ty = get_field_type_with_subst(fa, field_decl);

                    IrValue* base_ptr = it->second.value;
                    if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
                    {
                        auto* ptr_ir_type = lower_type(it->second.sema_type);
                        base_ptr = m_ctx.load(ptr_ir_type, base_ptr);
                        auto load_name = ident_name();
                        base_ptr->name = m_name_pool.back();
                        append_inst(base_ptr);
                    }

                    auto* gep = m_ctx.gep(m_ctx.pointer_to(lower_type(field_sema_ty)), base_ptr);
                    gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);

                    return lower_field_result(fa, gep, field_sema_ty);
                }

                if (auto* vd = ast::node_cast<ast::VarDecl>(resolved_obj))
                {
                    auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                    if (global)
                    {
                        auto* global_ptr = m_ctx.global_ref(global, m_ctx.pointer_to(global->type));
                        auto* field_decl = find_decl_field(fa);
                        if (!field_decl)
                            lower_panic(fa, "cannot find field decl");

                        std::uint32_t field_idx = field_decl->index;
                        auto* field_sema_ty = get_field_type_with_subst(fa, field_decl);

                        IrValue* base_ptr = global_ptr;
                        if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
                        {
                            base_ptr = m_ctx.load(global->type, global_ptr);
                            auto load_name = ident_name();
                            base_ptr->name = m_name_pool.back();
                            append_inst(base_ptr);
                        }

                        auto* gep = m_ctx.gep(m_ctx.pointer_to(lower_type(field_sema_ty)), base_ptr);
                        gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);

                        return lower_field_result(fa, gep, field_sema_ty);
                    }
                }
            }

            if (fa->object->sema.const_value)
            {
                auto* obj_val = materialize_comptime(*fa->object->sema.const_value, get_sema_resolved_type(fa->object));
                if (ir_cast<IrAggregateInst>(obj_val))
                {
                    auto* field_decl = find_decl_field(fa);
                    if (field_decl)
                    {
                        auto* extracted = m_ctx.extract(ir_resolved_type, obj_val, field_decl->index);
                        auto name = ident_name();
                        extracted->name = m_name_pool.back();
                        append_inst(extracted);
                        return extracted;
                    }
                }
            }

            auto* obj_val = lower_expr(fa->object);

            auto* field_decl = find_decl_field(fa);
            if (!field_decl)
                lower_panic(fa, "cannot find field decl");

            std::uint32_t field_idx = field_decl->index;

            if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
            {
                auto* field_sema_ty = get_field_type_with_subst(fa, field_decl);
                auto* gep = m_ctx.gep(m_ctx.pointer_to(lower_type(field_sema_ty)), obj_val);
                gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);
                return lower_field_result(fa, gep, field_sema_ty);
            }

            if (obj_val->kind == IrNodeKind::Load)
            {
                auto* extracted = m_ctx.extract(ir_resolved_type, obj_val, field_idx);
                auto name = ident_name();
                extracted->name = m_name_pool.back();
                append_inst(extracted);
                return extracted;
            }

            if (obj_val->type && (obj_val->type->kind == IrTypeKind::Aggregate || obj_val->type->kind == IrTypeKind::Array))
            {
                auto* extracted = m_ctx.extract(ir_resolved_type, obj_val, field_idx);
                auto name = ident_name();
                extracted->name = m_name_pool.back();
                append_inst(extracted);
                return extracted;
            }

            lower_panic(fa, "cannot lower field access");
        }

        ast::FieldDecl const* find_decl_field(ast::FieldAccessExpr const* fa)
        {
            auto* obj_type = get_sema_resolved_type(fa->object);
            if (!obj_type)
                return nullptr;

            if (auto* pt = types::type_cast<types::PointerType>(obj_type))
                obj_type = pt->pointee;

            if (auto* nt = types::type_cast<types::NominalType>(obj_type))
                obj_type = nt->underlying;

            ast::Decl const* decl = nullptr;
            if (auto* st = types::type_cast<types::StructType>(obj_type))
                decl = reinterpret_cast<ast::Decl const*>(st->decl);
            else if (auto* ut = types::type_cast<types::UnionType>(obj_type))
                decl = reinterpret_cast<ast::Decl const*>(ut->decl);

            if (!decl)
                return nullptr;

            if (auto* sd = ast::node_cast<ast::StructDecl>(decl))
            {
                for (auto& f : sd->fields)
                    if (f.name == fa->field)
                        return &f;
            }
            else if (auto* ud = ast::node_cast<ast::UnionDecl>(decl))
            {
                for (auto& f : ud->fields)
                    if (f.name == fa->field)
                        return &f;
            }
            return nullptr;
        }

        [[nodiscard]] bool is_fam_field(ast::FieldAccessExpr const* fa)
        {
            auto* field_decl = find_decl_field(fa);
            if (!field_decl || !field_decl->type)
                return false;

            auto* ft = get_canonical_type(field_decl->type);
            return types::is_fam_type(ft);
        }

        IrValue* lower_field_result(ast::FieldAccessExpr const* fa, IrValue* field_gep, dcc::types::TypePtr field_sema_type)
        {
            if (!types::is_fam_type(field_sema_type))
            {
                auto* ir_resolved_type = lower_type(get_sema_resolved_type(fa));
                auto* loaded = m_ctx.load(ir_resolved_type, field_gep);
                auto load_name = ident_name();
                loaded->name = m_name_pool.back();
                append_inst(loaded);
                return loaded;
            }

            return lower_fam_field_lvalue(fa, field_gep, field_sema_type);
        }

        IrValue* lower_fam_field_lvalue(ast::FieldAccessExpr const* fa, IrValue* field_gep, dcc::types::TypePtr field_sema_type)
        {
            auto* elem_type = types::fam_element(field_sema_type);
            if (!elem_type)
                lower_panic(fa, "FAM without element type");
            auto* ir_elem_type = lower_type(elem_type);
            auto* zero_idx = m_ctx.int_const(m_ctx.int_t(64, false), 0);
            auto* fam_gep = m_ctx.gep(m_ctx.pointer_to(ir_elem_type), field_gep);
            fam_gep->indices.push_back({IrGepInst::IndexKind::Array, zero_idx, 0});
            auto fam_gep_name = ident_name();
            fam_gep->name = m_name_pool.back();
            append_inst(fam_gep);
            return fam_gep;
        }

        IrValue* lower_index_expr(ast::IndexExpr const* idx_expr)
        {
            if (idx_expr->index && idx_expr->index->kind == ast::ExprKind::Range)
                return lower_range_slice_expr(idx_expr);

            auto* resolved_type = get_sema_resolved_type(idx_expr);
            auto* ir_resolved_type = lower_type(resolved_type);
            auto* obj_val = lower_expr(idx_expr->object);
            auto* index_val = lower_expr(idx_expr->index);

            auto* obj_sema_type = get_sema_resolved_type(idx_expr->object);

            if (obj_val->type && obj_val->type->kind == IrTypeKind::Slice)
            {
                auto* ptr_type = m_ctx.pointer_to(ir_resolved_type);
                auto* ptr_extract = m_ctx.extract(ptr_type, obj_val, 0);
                auto ptr_name = ident_name();
                ptr_extract->name = m_name_pool.back();
                append_inst(ptr_extract);

                auto* len_type = m_ctx.usize_t();
                auto* len_extract = m_ctx.extract(len_type, obj_val, 1);
                auto len_name = ident_name();
                len_extract->name = m_name_pool.back();
                append_inst(len_extract);

                if (m_bounds_check)
                    emit_bounds_check(ptr_extract, len_extract, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Slice);

                auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                auto* gep = m_ctx.gep(elem_ptr_type, ptr_extract);
                gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);

                auto* loaded = m_ctx.load(ir_resolved_type, gep);
                auto load_name = ident_name();
                loaded->name = m_name_pool.back();
                append_inst(loaded);
                return loaded;
            }

            auto* resolved =
                (idx_expr->object->kind == ast::ExprKind::Ident) ? static_cast<ast::IdentExpr const*>(idx_expr->object)->sema.resolved_decl : nullptr;

            if (resolved)
            {
                auto it = m_value_map.find(resolved);
                if (it != m_value_map.end() && it->second.is_storage)
                {
                    if (m_bounds_check && obj_sema_type && obj_sema_type->kind == types::TypeKind::Array)
                    {
                        auto const* at = types::type_cast<types::ArrayType>(obj_sema_type);
                        if (at)
                        {
                            bool skip_check = false;
                            if (auto* iconst = ir_cast<IrIntConstant>(index_val))
                                if (iconst->value >= 0 && static_cast<std::uint64_t>(iconst->value) < at->count)
                                    skip_check = true;

                            if (!skip_check)
                            {
                                auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(at->count));
                                emit_bounds_check(nullptr, len_val, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Array);
                            }
                        }
                    }

                    auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, it->second.value);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);

                    auto* loaded = m_ctx.load(ir_resolved_type, gep);
                    auto load_name = ident_name();
                    loaded->name = m_name_pool.back();
                    append_inst(loaded);
                    return loaded;
                }
            }

            if (auto* gr = ir_cast<IrGlobalRef>(obj_val))
            {
                if (gr->global)
                {
                    if (m_bounds_check && obj_sema_type && obj_sema_type->kind == types::TypeKind::Array)
                    {
                        auto const* at = types::type_cast<types::ArrayType>(obj_sema_type);
                        if (at)
                        {
                            bool skip_check = false;
                            if (auto* iconst = ir_cast<IrIntConstant>(index_val))
                                if (iconst->value >= 0 && static_cast<std::uint64_t>(iconst->value) < at->count)
                                    skip_check = true;

                            if (!skip_check)
                            {
                                auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(at->count));
                                emit_bounds_check(nullptr, len_val, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Array);
                            }
                        }
                    }

                    auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, const_cast<IrGlobalRef*>(gr));
                    gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);

                    auto* loaded = m_ctx.load(ir_resolved_type, gep);
                    auto load_name = ident_name();
                    loaded->name = m_name_pool.back();
                    append_inst(loaded);
                    return loaded;
                }
            }

            if (obj_val->type && (obj_val->type->kind == IrTypeKind::Aggregate || obj_val->type->kind == IrTypeKind::Array))
            {
                if (auto* iconst = ir_cast<IrIntConstant>(index_val))
                {
                    auto* extracted = m_ctx.extract(ir_resolved_type, obj_val, static_cast<std::uint32_t>(iconst->value));
                    auto name = ident_name();
                    extracted->name = m_name_pool.back();
                    append_inst(extracted);
                    return extracted;
                }
            }

            if (obj_val->type && obj_val->type->kind == IrTypeKind::Pointer)
            {
                auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                auto* gep = m_ctx.gep(elem_ptr_type, obj_val);
                gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);

                auto* loaded = m_ctx.load(ir_resolved_type, gep);
                auto load_name = ident_name();
                loaded->name = m_name_pool.back();
                append_inst(loaded);
                return loaded;
            }

            lower_panic(idx_expr, "cannot lower index expression - object is not a storage array");
        }

        IrValue* lower_range_slice_expr(ast::IndexExpr const* idx_expr)
        {
            auto& r = static_cast<ast::RangeExpr const&>(*idx_expr->index);
            auto* obj_sema_type = get_sema_resolved_type(idx_expr->object);
            auto* resolved_type = get_sema_resolved_type(idx_expr);
            auto* ir_slice_type = lower_type(resolved_type);

            if (!ir_slice_type || ir_slice_type->kind != IrTypeKind::Slice)
                lower_panic(idx_expr, "range slice resolved type is not a slice");

            auto* usize_type = m_ctx.usize_t();

            IrValue* source_len = nullptr;
            IrValue* source_ptr = nullptr;
            IrType const* ir_element_type = nullptr;

            if (auto const* at = types::type_cast<types::ArrayType>(obj_sema_type))
            {
                source_len = m_ctx.int_const(usize_type, static_cast<std::int64_t>(at->count));
                ir_element_type = lower_type(at->element);

                auto lv = lower_assign_lvalue(idx_expr->object);

                if (lv.entry && lv.entry->is_storage)
                {
                    auto* elem_ptr_type = m_ctx.pointer_to(ir_element_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, lv.entry->value);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(usize_type, 0), 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    source_ptr = gep;
                }
                else if (lv.gep_ptr)
                {
                    auto* elem_ptr_type = m_ctx.pointer_to(ir_element_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, lv.gep_ptr);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(usize_type, 0), 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    source_ptr = gep;
                }

                if (!source_ptr)
                {
                    auto* obj_val = lower_expr(idx_expr->object);
                    auto* arr_ptr_type = m_ctx.pointer_to(obj_val->type);
                    auto* temp = m_ctx.alloca(arr_ptr_type, obj_val->type);
                    auto temp_name = ident_name();
                    temp->name = m_name_pool.back();
                    append_inst(temp);
                    append_inst(m_ctx.store(obj_val, temp));

                    auto* elem_ptr_type = m_ctx.pointer_to(ir_element_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, temp);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, m_ctx.int_const(usize_type, 0), 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    source_ptr = gep;
                }
            }
            else if (auto const* st = types::type_cast<types::SliceType>(obj_sema_type))
            {
                ir_element_type = lower_type(st->element);

                auto* obj_val = lower_expr(idx_expr->object);

                auto* ptr_type = m_ctx.pointer_to(ir_element_type);
                auto* ptr_extract = m_ctx.extract(ptr_type, obj_val, 0);
                auto ptr_name = ident_name();
                ptr_extract->name = m_name_pool.back();
                append_inst(ptr_extract);
                source_ptr = ptr_extract;

                auto* len_extract = m_ctx.extract(usize_type, obj_val, 1);
                auto len_name = ident_name();
                len_extract->name = m_name_pool.back();
                append_inst(len_extract);
                source_len = len_extract;
            }
            else
            {
                lower_panic(idx_expr, "cannot range-slice non-array/non-slice type");
            }

            IrValue* start_val = nullptr;
            IrValue* end_val = nullptr;

            auto ensure_usize = [&](IrValue* v) -> IrValue* {
                if (!v || !v->type)
                    return v;

                if (v->type->kind == IrTypeKind::Int)
                {
                    auto* int_ty = static_cast<IrIntType const*>(v->type);
                    if (int_ty->bits != m_ctx.pointer_bits() || int_ty->is_signed || !int_ty->is_pointer_sized)
                    {
                        if (auto* ic = ir_cast<IrIntConstant>(v))
                            return m_ctx.int_const(usize_type, ic->value);

                        auto* casted = m_ctx.zext(usize_type, v);
                        auto name = ident_name();
                        casted->name = m_name_pool.back();
                        append_inst(casted);
                        return casted;
                    }
                }
                return v;
            };

            if (r.start)
                start_val = ensure_usize(lower_expr(r.start));
            else
                start_val = m_ctx.int_const(usize_type, 0);

            if (r.end)
                end_val = ensure_usize(lower_expr(r.end));
            else
                end_val = source_len;

            IrValue* effective_end = nullptr;
            if (r.inclusive)
            {
                auto* one = m_ctx.int_const(usize_type, 1);
                effective_end = m_ctx.add(usize_type, end_val, one);
                auto eff_name = ident_name();
                effective_end->name = m_name_pool.back();
                append_inst(effective_end);
            }
            else
            {
                effective_end = end_val;
            }

            if (m_bounds_check)
            {
                auto ast_int_value = [](ast::Expr const* e) -> std::optional<std::int64_t> {
                    if (!e)
                        return std::nullopt;
                    if (auto* il = ast::node_cast<ast::IntLiteralExpr>(e))
                        return il->value;
                    if (auto* cl = ast::node_cast<ast::CharLiteralExpr>(e))
                        return static_cast<std::int64_t>(cl->codepoint);
                    if (auto* ucl = ast::node_cast<ast::U16CharLiteralExpr>(e))
                        return static_cast<std::int64_t>(ucl->value);
                    if (e->sema.const_value)
                    {
                        auto v = e->sema.const_value->const_to_int();
                        if (v)
                            return v;
                    }
                    return std::nullopt;
                };

                std::optional<std::int64_t> ast_start;
                if (r.start)
                    ast_start = ast_int_value(r.start);
                else
                    ast_start = 0;

                std::optional<std::int64_t> ast_end = ast_int_value(r.end);
                std::optional<std::int64_t> ast_effective_end;

                if (ast_end)
                {
                    if (r.inclusive)
                        ast_effective_end = *ast_end + 1;
                    else
                        ast_effective_end = *ast_end;
                }
                else if (!r.end)
                {
                    if (auto const* at = types::type_cast<types::ArrayType>(obj_sema_type))
                        ast_effective_end = static_cast<std::int64_t>(at->count);
                }

                bool need_start_le_end = true;
                if (ast_start && ast_effective_end && *ast_start >= 0 && *ast_effective_end >= 0)
                {
                    if (*ast_start <= *ast_effective_end)
                        need_start_le_end = false;
                }
                else if (ast_start && *ast_start == 0 && !r.end)
                    need_start_le_end = false;

                bool need_end_le_len = true;
                if (!r.end)
                    need_end_le_len = false;
                else if (ast_effective_end && *ast_effective_end >= 0)
                {
                    if (auto const* at = types::type_cast<types::ArrayType>(obj_sema_type))
                    {
                        if (*ast_effective_end <= static_cast<std::int64_t>(at->count))
                            need_end_le_len = false;
                    }
                }

                if (need_start_le_end || need_end_le_len)
                {
                    std::string file = "<unknown>";
                    int line = 0;
                    std::string func = "<unknown>";

                    if (m_current_func_decl && !m_current_func_decl->name.empty())
                        func = std::string(m_current_func_decl->name);
                    else if (m_current_func && !m_current_func->name.empty())
                        func = std::string(m_current_func->name);

                    sm::SourceRange resolved_range = idx_expr->range;
                    if (!resolved_range.valid() && m_current_func_decl)
                        resolved_range = m_current_func_decl->range;

                    auto* sm_ptr = get_source_manager();
                    if (sm_ptr && resolved_range.valid())
                    {
                        auto const* f = sm_ptr->get(resolved_range.begin.fileId);
                        if (f)
                            file = f->path().filename().string();
                        auto lc = sm_ptr->line_col(resolved_range.begin);
                        if (lc)
                            line = static_cast<int>(lc->line);
                    }

                    auto emit_one_check = [&](IrValue* lhs, IrValue* rhs, std::string_view suffix) {
                        auto* cmp = m_ctx.cmp_ule(lhs, rhs);
                        auto cmp_name = ident_name();
                        cmp->name = m_name_pool.back();
                        append_inst(cmp);

                        auto* ok_bb = create_block(std::string{"slice.ok"} + std::string{suffix});
                        auto* fail_bb = create_block(std::string{"slice.fail"} + std::string{suffix});

                        emit_br_cond(cmp, ok_bb, fail_bb);

                        set_current_block(fail_bb);
                        emit_assert_call(file, line, func, "slice range out of bounds");
                        emit_unreachable();

                        set_current_block(ok_bb);
                    };

                    if (need_start_le_end)
                        emit_one_check(start_val, effective_end, "a");

                    if (need_end_le_len)
                        emit_one_check(effective_end, source_len, "b");
                }
            }

            auto* slice_len = m_ctx.sub(usize_type, effective_end, start_val);
            auto slice_len_name = ident_name();
            slice_len->name = m_name_pool.back();
            append_inst(slice_len);

            auto* elem_ptr_type = m_ctx.pointer_to(ir_element_type);
            auto* gep = m_ctx.gep(elem_ptr_type, source_ptr);
            gep->indices.push_back({IrGepInst::IndexKind::Array, start_val, 0});

            auto gep_name = ident_name();
            gep->name = m_name_pool.back();
            append_inst(gep);

            auto* agg = m_ctx.aggregate(ir_slice_type);
            agg->values.push_back(gep);
            agg->values.push_back(slice_len);
            auto agg_name = ident_name();
            agg->name = m_name_pool.back();
            append_inst(agg);

            return agg;
        }

        IrValue* lower_field_lvalue(ast::FieldAccessExpr const* fa)
        {
            auto* resolved_type = get_sema_resolved_type(fa);
            auto* ir_resolved_type = lower_type(resolved_type);

            auto* obj_sema_type = get_sema_resolved_type(fa->object);
            if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Slice)
            {
                std::uint32_t field_idx = (fa->field == "ptr") ? 0 : (fa->field == "len") ? 1 : std::numeric_limits<std::uint32_t>::max();
                if (field_idx > 1)
                    lower_panic(fa, std::format("unknown slice field for lvalue: {}", fa->field));

                auto* resolved = (fa->object->kind == ast::ExprKind::Ident) ? static_cast<ast::IdentExpr const*>(fa->object)->sema.resolved_decl : nullptr;

                if (resolved)
                {
                    auto it = m_value_map.find(resolved);
                    if (it != m_value_map.end() && it->second.is_storage)
                    {
                        auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* gep = m_ctx.gep(elem_ptr_type, it->second.value);
                        gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);
                        return gep;
                    }

                    if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                    {
                        auto* global = get_or_create_global_ref(vd);
                        if (global)
                        {
                            auto* ptr_type = m_ctx.pointer_to(global->type);
                            auto* global_ref = m_ctx.global_ref(global, ptr_type);
                            auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                            auto* gep = m_ctx.gep(elem_ptr_type, global_ref);
                            gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                            auto gep_name = ident_name();
                            gep->name = m_name_pool.back();
                            append_inst(gep);
                            return gep;
                        }
                    }
                }

                lower_panic(fa, "cannot lower slice field lvalue");
            }

            auto* field_decl = find_decl_field(fa);
            if (!field_decl)
                lower_panic(fa, "cannot find field decl for lvalue");

            std::uint32_t field_idx = field_decl->index;
            auto* field_sema_ty = get_field_type_with_subst(fa, field_decl);
            bool is_fam_lv = types::is_fam_type(field_sema_ty);

            auto wrap_lvalue = [&](IrValue* gep) -> IrValue* {
                if (is_fam_lv)
                    return lower_fam_field_lvalue(fa, gep, field_sema_ty);
                return gep;
            };

            auto* resolved = (fa->object->kind == ast::ExprKind::Ident) ? static_cast<ast::IdentExpr const*>(fa->object)->sema.resolved_decl : nullptr;

            if (resolved)
            {
                auto it = m_value_map.find(resolved);
                if (it != m_value_map.end() && it->second.is_storage)
                {
                    IrValue* base_ptr = it->second.value;
                    if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
                    {
                        auto* ptr_ir_type = lower_type(it->second.sema_type);
                        base_ptr = m_ctx.load(ptr_ir_type, base_ptr);
                        auto load_name = ident_name();
                        base_ptr->name = m_name_pool.back();
                        append_inst(base_ptr);
                    }

                    auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                    auto* gep = m_ctx.gep(elem_ptr_type, base_ptr);
                    gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return wrap_lvalue(gep);
                }

                if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                {
                    auto* global = get_or_create_global_ref(const_cast<ast::VarDecl*>(vd));
                    if (global)
                    {
                        auto* ptr_type = m_ctx.pointer_to(global->type);
                        auto* global_ref = m_ctx.global_ref(global, ptr_type);
                        IrValue* base_ptr = global_ref;
                        if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
                        {
                            base_ptr = m_ctx.load(global->type, global_ref);
                            auto load_name = ident_name();
                            base_ptr->name = m_name_pool.back();
                            append_inst(base_ptr);
                        }

                        auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                        auto* gep = m_ctx.gep(elem_ptr_type, base_ptr);
                        gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);
                        return wrap_lvalue(gep);
                    }
                }
            }

            if (fa->object->kind == ast::ExprKind::Index)
            {
                auto* base_ptr = lower_index_lvalue(static_cast<ast::IndexExpr const*>(fa->object));
                auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                auto* gep = m_ctx.gep(elem_ptr_type, base_ptr);
                gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);
                return wrap_lvalue(gep);
            }

            if (fa->object->kind == ast::ExprKind::FieldAccess)
            {
                auto* inner_ptr = lower_field_lvalue(static_cast<ast::FieldAccessExpr const*>(fa->object));
                auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                auto* gep = m_ctx.gep(elem_ptr_type, inner_ptr);
                gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);
                return wrap_lvalue(gep);
            }

            if (fa->object->kind == ast::ExprKind::Unary)
            {
                auto* ue = static_cast<ast::UnaryExpr const*>(fa->object);
                if (ue->op == dcc::lex::TokenKind::Star)
                {
                    auto* ptr = lower_expr(ue->operand);
                    auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                    auto* gep = m_ctx.gep(elem_ptr_type, ptr);
                    gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return wrap_lvalue(gep);
                }
            }

            auto* obj_val = lower_expr(fa->object);

            if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
            {
                auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                auto* gep = m_ctx.gep(elem_ptr_type, obj_val);
                gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                auto gep_name = ident_name();
                gep->name = m_name_pool.back();
                append_inst(gep);
                return wrap_lvalue(gep);
            }

            if (auto* gr = ir_cast<IrGlobalRef>(obj_val))
            {
                if (gr->global)
                {
                    auto* elem_ptr_type = m_ctx.pointer_to(lower_type(field_sema_ty));
                    auto* gep = m_ctx.gep(elem_ptr_type, const_cast<IrGlobalRef*>(gr));
                    gep->indices.push_back({IrGepInst::IndexKind::Field, nullptr, field_idx});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return wrap_lvalue(gep);
                }
            }

            lower_panic(fa, "cannot lower field lvalue");
        }

        IrValue* lower_index_lvalue(ast::IndexExpr const* idx_expr)
        {
            if (idx_expr->index && idx_expr->index->kind == ast::ExprKind::Range)
                lower_panic(idx_expr, "cannot take lvalue of range slice");

            auto* resolved_type = get_sema_resolved_type(idx_expr);
            auto* ir_resolved_type = lower_type(resolved_type);
            auto* index_val = lower_expr(idx_expr->index);

            auto* obj_sema_type = get_sema_resolved_type(idx_expr->object);

            auto* resolved =
                (idx_expr->object->kind == ast::ExprKind::Ident) ? static_cast<ast::IdentExpr const*>(idx_expr->object)->sema.resolved_decl : nullptr;

            if (resolved)
            {
                auto it = m_value_map.find(resolved);
                if (it != m_value_map.end() && it->second.is_storage)
                {
                    if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Slice)
                    {
                        auto* ptr_to_field = m_ctx.pointer_to(m_ctx.pointer_to(ir_resolved_type));
                        auto* gep_ptr = m_ctx.gep(ptr_to_field, it->second.value);
                        gep_ptr->indices.push_back({IrGepInst::IndexKind::Field, nullptr, 0});
                        auto gep_ptr_name = ident_name();
                        gep_ptr->name = m_name_pool.back();
                        append_inst(gep_ptr);

                        auto* ptr_val = m_ctx.load(m_ctx.pointer_to(ir_resolved_type), gep_ptr);
                        auto ptr_load_name = ident_name();
                        ptr_val->name = m_name_pool.back();
                        append_inst(ptr_val);

                        if (m_bounds_check)
                        {
                            auto* ptr_to_len_field = m_ctx.pointer_to(m_ctx.usize_t());
                            auto* gep_len = m_ctx.gep(ptr_to_len_field, it->second.value);
                            gep_len->indices.push_back({IrGepInst::IndexKind::Field, nullptr, 1});
                            auto gep_len_name = ident_name();
                            gep_len->name = m_name_pool.back();
                            append_inst(gep_len);

                            auto* len_val = m_ctx.load(m_ctx.usize_t(), gep_len);
                            auto len_load_name = ident_name();
                            len_val->name = m_name_pool.back();
                            append_inst(len_val);

                            emit_bounds_check(ptr_val, len_val, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Slice);
                        }

                        auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* gep = m_ctx.gep(elem_ptr_type, ptr_val);
                        gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);
                        return gep;
                    }

                    if (m_bounds_check && obj_sema_type && obj_sema_type->kind == types::TypeKind::Array)
                    {
                        auto const* at = types::type_cast<types::ArrayType>(obj_sema_type);
                        if (at)
                        {
                            bool skip_check = false;
                            if (auto* iconst = ir_cast<IrIntConstant>(index_val))
                                if (iconst->value >= 0 && static_cast<std::uint64_t>(iconst->value) < at->count)
                                    skip_check = true;

                            if (!skip_check)
                            {
                                auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(at->count));
                                emit_bounds_check(nullptr, len_val, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Array);
                            }
                        }
                    }

                    auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, it->second.value);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return gep;
                }

                if (auto* vd = ast::node_cast<ast::VarDecl>(resolved))
                {
                    auto* global = get_or_create_global_ref(vd);
                    if (global)
                    {
                        if (m_bounds_check && obj_sema_type && obj_sema_type->kind == types::TypeKind::Array)
                        {
                            auto const* at = types::type_cast<types::ArrayType>(obj_sema_type);
                            if (at)
                            {
                                bool skip_check = false;
                                if (auto* iconst = ir_cast<IrIntConstant>(index_val))
                                    if (iconst->value >= 0 && static_cast<std::uint64_t>(iconst->value) < at->count)
                                        skip_check = true;

                                if (!skip_check)
                                {
                                    auto* len_val = m_ctx.int_const(m_ctx.usize_t(), static_cast<std::int64_t>(at->count));
                                    emit_bounds_check(nullptr, len_val, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Array);
                                }
                            }
                        }

                        auto* ptr_type = m_ctx.pointer_to(global->type);
                        auto* global_ref = m_ctx.global_ref(global, ptr_type);
                        auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* gep = m_ctx.gep(elem_ptr_type, global_ref);
                        gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);
                        return gep;
                    }
                }
            }

            {
                auto it = m_value_map.find(resolved);
                if (it != m_value_map.end() && !it->second.is_storage)
                {
                    auto* entry_val = it->second.value;
                    if (entry_val->type && entry_val->type->kind == IrTypeKind::Slice)
                    {
                        auto* ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* ptr_extract = m_ctx.extract(ptr_type, entry_val, 0);
                        auto ptr_name = ident_name();
                        ptr_extract->name = m_name_pool.back();
                        append_inst(ptr_extract);

                        if (m_bounds_check)
                        {
                            auto* len_extract = m_ctx.extract(m_ctx.usize_t(), entry_val, 1);
                            auto len_name = ident_name();
                            len_extract->name = m_name_pool.back();
                            append_inst(len_extract);
                            emit_bounds_check(ptr_extract, len_extract, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Slice);
                        }

                        auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* gep = m_ctx.gep(elem_ptr_type, ptr_extract);
                        gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);
                        return gep;
                    }

                    if (entry_val->type && entry_val->type->kind == IrTypeKind::Pointer)
                    {
                        auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                        auto* gep = m_ctx.gep(elem_ptr_type, entry_val);
                        gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                        auto gep_name = ident_name();
                        gep->name = m_name_pool.back();
                        append_inst(gep);
                        return gep;
                    }
                }
            }

            {
                auto* obj_val = lower_expr(idx_expr->object);

                if (obj_sema_type && obj_sema_type->kind == types::TypeKind::Pointer)
                {
                    auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, obj_val);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return gep;
                }

                if (obj_val->type && obj_val->type->kind == IrTypeKind::Slice)
                {
                    auto* ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* ptr_extract = m_ctx.extract(ptr_type, obj_val, 0);
                    auto ptr_name = ident_name();
                    ptr_extract->name = m_name_pool.back();
                    append_inst(ptr_extract);

                    if (m_bounds_check)
                    {
                        auto* len_extract = m_ctx.extract(m_ctx.usize_t(), obj_val, 1);
                        auto len_name = ident_name();
                        len_extract->name = m_name_pool.back();
                        append_inst(len_extract);
                        emit_bounds_check(ptr_extract, len_extract, index_val, idx_expr->range, idx_expr->index->range, BoundsCheckKind::Slice);
                    }

                    auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, ptr_extract);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return gep;
                }

                if (obj_val->type && obj_val->type->kind == IrTypeKind::Pointer)
                {
                    auto* elem_ptr_type = m_ctx.pointer_to(ir_resolved_type);
                    auto* gep = m_ctx.gep(elem_ptr_type, obj_val);
                    gep->indices.push_back({IrGepInst::IndexKind::Array, index_val, 0});
                    auto gep_name = ident_name();
                    gep->name = m_name_pool.back();
                    append_inst(gep);
                    return gep;
                }
            }

            lower_panic(idx_expr, "cannot lower index lvalue");
        }

        std::list<std::string> m_nominal_names;
        std::list<std::vector<std::string>> m_nominal_paths;
    };

} // namespace dcc::ir::lower
