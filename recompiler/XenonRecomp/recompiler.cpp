//#include "pch.h"
#include "recompiler.h"
#include <xex_patcher.h>
#include <sstream>

static uint64_t ComputeMask(uint32_t mstart, uint32_t mstop)
{
    mstart &= 0x3F;
    mstop &= 0x3F;
    uint64_t value = (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
    return mstart <= mstop ? value : ~value;
}

bool Recompiler::LoadConfig(const std::string_view& configFilePath)
{
    config.Load(configFilePath);

    std::vector<uint8_t> file;
    if (!config.patchedFilePath.empty())
        file = LoadFile((config.directoryPath + config.patchedFilePath).c_str());

    if (file.empty())
    {
        file = LoadFile((config.directoryPath + config.filePath).c_str());

        if (!config.patchFilePath.empty())
        {
            const auto patchFile = LoadFile((config.directoryPath + config.patchFilePath).c_str());
            if (!patchFile.empty())
            {
                std::vector<uint8_t> outBytes;
                auto result = XexPatcher::apply(file.data(), file.size(), patchFile.data(), patchFile.size(), outBytes, false);
                if (result == XexPatcher::Result::Success)
                {
                    std::exchange(file, outBytes);

                    if (!config.patchedFilePath.empty())
                    {
                        std::ofstream stream(config.directoryPath + config.patchedFilePath, std::ios::binary);
                        if (stream.good())
                        {
                            stream.write(reinterpret_cast<const char*>(file.data()), file.size());
                            stream.close();
                        }
                    }
                }
                else
                {
                    fmt::print("ERROR: Unable to apply the patch file, ");

                    switch (result)
                    {
                    case XexPatcher::Result::XexFileUnsupported:
                        fmt::println("XEX file unsupported");
                        break;

                    case XexPatcher::Result::XexFileInvalid:
                        fmt::println("XEX file invalid");
                        break;

                    case XexPatcher::Result::PatchFileInvalid:
                        fmt::println("patch file invalid");
                        break;

                    case XexPatcher::Result::PatchIncompatible:
                        fmt::println("patch file incompatible");
                        break;

                    case XexPatcher::Result::PatchFailed:
                        fmt::println("patch failed");
                        break;

                    case XexPatcher::Result::PatchUnsupported:
                        fmt::println("patch unsupported");
                        break;

                    default:
                        fmt::println("reason unknown");
                        break;
                    }

                    return false;
                }
            }
            else
            {
                fmt::println("ERROR: Unable to load the patch file");
                return false;
            }
        }
    }

    image = Image::ParseImage(file.data(), file.size());
    return true;
}

void Recompiler::Analyse()
{
    for (size_t i = 14; i < 128; i++)
    {
        if (i < 32)
        {
            if (config.restGpr14Address != 0)
            {
                auto& restgpr = functions.emplace_back();
                restgpr.base = config.restGpr14Address + (i - 14) * 4;
                restgpr.size = (32 - i) * 4 + 12;
                image.symbols.emplace(Symbol{ fmt::format("__restgprlr_{}", i), restgpr.base, restgpr.size, Symbol_Function });
            }

            if (config.saveGpr14Address != 0)
            {
                auto& savegpr = functions.emplace_back();
                savegpr.base = config.saveGpr14Address + (i - 14) * 4;
                savegpr.size = (32 - i) * 4 + 8;
                image.symbols.emplace(fmt::format("__savegprlr_{}", i), savegpr.base, savegpr.size, Symbol_Function);
            }

            if (config.restFpr14Address != 0)
            {
                auto& restfpr = functions.emplace_back();
                restfpr.base = config.restFpr14Address + (i - 14) * 4;
                restfpr.size = (32 - i) * 4 + 4;
                image.symbols.emplace(fmt::format("__restfpr_{}", i), restfpr.base, restfpr.size, Symbol_Function);
            }

            if (config.saveFpr14Address != 0)
            {
                auto& savefpr = functions.emplace_back();
                savefpr.base = config.saveFpr14Address + (i - 14) * 4;
                savefpr.size = (32 - i) * 4 + 4;
                image.symbols.emplace(fmt::format("__savefpr_{}", i), savefpr.base, savefpr.size, Symbol_Function);
            }

            if (config.restVmx14Address != 0)
            {
                auto& restvmx = functions.emplace_back();
                restvmx.base = config.restVmx14Address + (i - 14) * 8;
                restvmx.size = (32 - i) * 8 + 4;
                image.symbols.emplace(fmt::format("__restvmx_{}", i), restvmx.base, restvmx.size, Symbol_Function);
            }

            if (config.saveVmx14Address != 0)
            {
                auto& savevmx = functions.emplace_back();
                savevmx.base = config.saveVmx14Address + (i - 14) * 8;
                savevmx.size = (32 - i) * 8 + 4;
                image.symbols.emplace(fmt::format("__savevmx_{}", i), savevmx.base, savevmx.size, Symbol_Function);
            }
        }

        if (i >= 64)
        {
            if (config.restVmx64Address != 0)
            {
                auto& restvmx = functions.emplace_back();
                restvmx.base = config.restVmx64Address + (i - 64) * 8;
                restvmx.size = (128 - i) * 8 + 4;
                image.symbols.emplace(fmt::format("__restvmx_{}", i), restvmx.base, restvmx.size, Symbol_Function);
            }

            if (config.saveVmx64Address != 0)
            {
                auto& savevmx = functions.emplace_back();
                savevmx.base = config.saveVmx64Address + (i - 64) * 8;
                savevmx.size = (128 - i) * 8 + 4;
                image.symbols.emplace(fmt::format("__savevmx_{}", i), savevmx.base, savevmx.size, Symbol_Function);
            }
        }
    }

    for (auto& [address, size] : config.functions)
    {
        functions.emplace_back(address, size);
        image.symbols.emplace(fmt::format("sub_{:X}", address), address, size, Symbol_Function);
    }

    auto& pdata = *image.Find(".pdata");
    size_t count = pdata.size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
    auto* pf = (IMAGE_CE_RUNTIME_FUNCTION*)pdata.data;
    for (size_t i = 0; i < count; i++)
    {
        auto fn = pf[i];
        fn.BeginAddress = ByteSwap(fn.BeginAddress);
        fn.Data = ByteSwap(fn.Data);

        if (image.symbols.find(fn.BeginAddress) == image.symbols.end())
        {
            auto& f = functions.emplace_back();
            f.base = fn.BeginAddress;
            f.size = fn.FunctionLength * 4;

            image.symbols.emplace(fmt::format("sub_{:X}", f.base), f.base, f.size, Symbol_Function);
        }
    }

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
        {
            continue;
        }
        size_t base = section.base;
        uint8_t* data = section.data;
        uint8_t* dataEnd = section.data + section.size;

        fprintf(stderr, "[analyse] section %s base=0x%zX size=0x%X: bl-target pass\n", section.name.c_str(), section.base, section.size);
        fflush(stderr);

        while (data < dataEnd)
        {
            uint32_t insn = ByteSwap(*(uint32_t*)data);
            if (PPC_OP(insn) == PPC_OP_B && PPC_BL(insn))
            {
                size_t address = base + (data - section.data) + PPC_BI(insn);

                if (address >= section.base && address < section.base + section.size && image.symbols.find(address) == image.symbols.end())
                {
                    auto data = section.data + address - section.base;
                    auto& fn = functions.emplace_back(Function::Analyze(data, section.base + section.size - address, address));
                    image.symbols.emplace(fmt::format("sub_{:X}", fn.base), fn.base, fn.size, Symbol_Function);
                }
            }
            data += 4;
        }

        data = section.data;

        fprintf(stderr, "[analyse] section %s: linear sweep (funcs so far: %zu)\n", section.name.c_str(), functions.size());
        fflush(stderr);

        size_t lastReport = 0;
        while (data < dataEnd)
        {
            const size_t off = data - section.data;
            if (off - lastReport >= 0x10000)
            {
                lastReport = off;
                fprintf(stderr, "[analyse] sweep at +0x%zX / 0x%X (funcs: %zu)\n", off, section.size, functions.size());
                fflush(stderr);
            }
            auto invalidInstr = config.invalidInstructions.find(ByteSwap(*(uint32_t*)data));
            if (invalidInstr != config.invalidInstructions.end())
            {
                base += invalidInstr->second;
                data += invalidInstr->second;
                continue;
            }

            auto fnSymbol = image.symbols.find(base);
            if (fnSymbol != image.symbols.end() && fnSymbol->address == base && (fnSymbol->type == Symbol_Function || fnSymbol->type == Symbol_Import))
            {
                assert(fnSymbol->address == base);

                base += fnSymbol->size;
                data += fnSymbol->size;
            }
            else
            {
                fprintf(stderr, "[analyse] sweep Analyze at 0x%zX\n", base);
                fflush(stderr);
                auto& fn = functions.emplace_back(Function::Analyze(data, dataEnd - data, base));
                image.symbols.emplace(fmt::format("sub_{:X}", fn.base), fn.base, fn.size, Symbol_Function);

                base += fn.size;
                data += fn.size;
            }
        }
    }

    std::sort(functions.begin(), functions.end(), [](auto& lhs, auto& rhs) { return lhs.base < rhs.base; });
}

bool Recompiler::Recompile(
    const Function& fn,
    uint32_t base,
    const ppc_insn& insn,
    const uint32_t* data,
    std::unordered_map<uint32_t, RecompilerSwitchTable>::iterator& switchTable,
    RecompilerLocalVariables& localVariables,
    CSRState& csrState)
{
    println("\t// {:X} {} {}", base, insn.opcode->name, insn.op_str);

    // TODO: we could cache these formats in an array
    auto r = [&](size_t index)
        {
            if ((config.nonArgumentRegistersAsLocalVariables && (index == 0 || index == 2 || index == 11 || index == 12)) ||
                (config.nonVolatileRegistersAsLocalVariables && index >= 14))
            {
                localVariables.r[index] = true;
                return fmt::format("r{}", index);
            }
            return fmt::format("ADJ(ctx)->r{}", index);
        };

    auto f = [&](size_t index)
        {
            if ((config.nonArgumentRegistersAsLocalVariables && index == 0) ||
                (config.nonVolatileRegistersAsLocalVariables && index >= 14))
            {
                localVariables.f[index] = true;
                return fmt::format("f{}", index);
            }
            return fmt::format("ADJ(ctx)->f{}", index);
        };

    auto v = [&](size_t index)
        {
            if ((config.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
                (config.nonVolatileRegistersAsLocalVariables && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127))))
            {
                localVariables.v[index] = true;
                return fmt::format("v{}", index);
            }
            return fmt::format("ADJ(ctx)->v{}", index);
        };

    auto cr = [&](size_t index)
        {
            if (config.crRegistersAsLocalVariables)
            {
                localVariables.cr[index] = true;
                return fmt::format("cr{}", index);
            }
            return fmt::format("ADJ(ctx)->cr{}", index);
        };

    auto ctr = [&]()
        {
            if (config.ctrAsLocalVariable)
            {
                localVariables.ctr = true;
                return "ctr";
            }
            return "ADJ(ctx)->ctr";
        };

    auto xer = [&]()
        {
            if (config.xerAsLocalVariable)
            {
                localVariables.xer = true;
                return "xer";
            }
            return "ADJ(ctx)->xer";
        };

    auto reserved = [&]()
        {
            if (config.reservedRegisterAsLocalVariable)
            {
                localVariables.reserved = true;
                return "reserved";
            }
            return "ADJ(ctx)->reserved";
        };

    auto temp = [&]()
        {
            localVariables.temp = true;
            return "temp";
        };

    auto vTemp = [&]()
        {
            localVariables.vTemp = true;
            return "vTemp";
        };

    auto env = [&]()
        {
            localVariables.env = true;
            return "env";
        };

    auto ea = [&]()
        {
            localVariables.ea = true;
            return "ea";
        };

    // TODO (Sajid): Check for out of bounds access
    auto mmioStore = [&]() -> bool
        {
            return config.mmioInstrs.find(base) != config.mmioInstrs.end();
        };

    auto printFunctionCall = [&](uint32_t address)
        {
            // s35 (XEO3 ABI): ctx is a uintptr_t here, not a PPCContext&, so the
            // upstream `env = ctx; ... ctx = env` form cannot compile. Both are macros in
            // ppc_context.h: PPC_SETJMP() runs the HOST setjmp in this frame (the only
            // frame that survives), snapshots the guest register file and puts it back
            // when a longjmp lands here; PPC_LONGJMP() is the host longjmp. Without them
            // the guest longjmp is an ordinary function whose `blr` just returns to its
            // caller -- Lua's luaD_throw then falls through into panic()/exit(), which is
            // what killed the dashboard on its first caught-on-hardware Lua error (s33/s35).
            if (address == config.longJmpAddress)
            {
                println("\tPPC_LONGJMP();");
            }
            else if (address == config.setJmpAddress)
            {
                println("\tPPC_SETJMP();");
            }
            else
            {
                auto targetSymbol = image.symbols.find(address);

                if (targetSymbol != image.symbols.end() && targetSymbol->address == address && (targetSymbol->type == Symbol_Function || targetSymbol->type == Symbol_Import))
                {
                    if (config.nonVolatileRegistersAsLocalVariables && (targetSymbol->name.find("__rest") == 0 || targetSymbol->name.find("__save") == 0))
                    {
                        // print nothing
                    }
                    else
                    {
                        println("\t{}(ctx, base, frame);", targetSymbol->name);
                    }
                }
                else
                {
                    println("\t// ERROR {:X}", address);
                }
            }
        };

    auto printConditionalBranch = [&](bool not_, const std::string_view& cond)
        {
            if (insn.operands[1] < fn.base || insn.operands[1] >= fn.base + fn.size)
            {
                println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
                print("\t");
                printFunctionCall(insn.operands[1]);
                println("\t\tPPC_RETURN();");
                println("\t}}");
            }
            else
            {
                println("\tif ({}{}.{}) goto loc_{:X};", not_ ? "!" : "", cr(insn.operands[0]), cond, insn.operands[1]);
            }
        };

    auto printSetFlushMode = [&](bool enable)
        {
            auto newState = enable ? CSRState::VMX : CSRState::FPU;
            if (csrState != newState)
            {
                auto prefix = enable ? "enable" : "disable";
                auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
                println("\tADJ(ctx)->fpscr.{}FlushMode{}();", prefix, suffix);

                csrState = newState;
            }
        };

    auto midAsmHook = config.midAsmHooks.find(base);

    auto printMidAsmHook = [&]()
        {
            bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                midAsmHook->second.jumpAddressOnFalse != NULL || midAsmHook->second.jumpAddressOnTrue != NULL;

            print("\t");
            if (returnsBool)
                print("if (");

            print("{}(", midAsmHook->second.name);
            for (auto& reg : midAsmHook->second.registers)
            {
                if (out.back() != '(')
                    out += ", ";

                switch (reg[0])
                {
                case 'c':
                    if (reg == "ctr")
                        out += ctr();
                    else
                        out += cr(std::atoi(reg.c_str() + 2));
                    break;

                case 'x':
                    out += xer();
                    break;

                case 'r':
                    if (reg == "reserved")
                        out += reserved();
                    else
                        out += r(std::atoi(reg.c_str() + 1));
                    break;

                case 'f':
                    if (reg == "fpscr")
                        out += "ADJ(ctx)->fpscr";
                    else
                        out += f(std::atoi(reg.c_str() + 1));
                    break;

                case 'v':
                    out += v(std::atoi(reg.c_str() + 1));
                    break;
                }
            }

            if (returnsBool)
            {
                println(")) {{");

                if (midAsmHook->second.returnOnTrue)
                    println("\t\tPPC_RETURN();");
                else if (midAsmHook->second.jumpAddressOnTrue != NULL)
                    println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

                println("\t}}");

                println("\telse {{");

                if (midAsmHook->second.returnOnFalse)
                    println("\t\tPPC_RETURN();");
                else if (midAsmHook->second.jumpAddressOnFalse != NULL)
                    println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

                println("\t}}");
            }
            else
            {
                println(");");

                if (midAsmHook->second.ret)
                    println("\tPPC_RETURN();");
                else if (midAsmHook->second.jumpAddress != NULL)
                    println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
            }
        };

    if (midAsmHook != config.midAsmHooks.end() && !midAsmHook->second.afterInstruction)
        printMidAsmHook();

    int id = insn.opcode->id;

    // Handling instructions that don't disassemble correctly for some reason here
    if (id == PPC_INST_VUPKHSB128 && insn.operands[2] == 0x60) id = PPC_INST_VUPKHSH128;
    else if (id == PPC_INST_VUPKLSB128 && insn.operands[2] == 0x60) id = PPC_INST_VUPKLSH128;

  
   

            switch (id)
            {
                {
            case PPC_INST_ADD:
                println("\t{}.u64 = {}.u64 + {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;
                }


            case PPC_INST_ADDC:
                println("\t{}.ca = ({}.u32 + {}.u32 < {}.u32);", xer(), r(insn.operands[1]), r(insn.operands[2]), r(insn.operands[1]));
                println("\t{}.u64 = {}.u64 + {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ADDE:
                println("\t{}.u8 = ({}.u32 + {}.u32 < {}.u32) | ({}.u32 + {}.u32 + {}.ca < {}.ca);", temp(), r(insn.operands[1]), r(insn.operands[2]), r(insn.operands[1]), r(insn.operands[1]), r(insn.operands[2]), xer(), xer());
                println("\t{}.u64 = {}.u64 + {}.u64 + {}.ca;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]), xer());
                println("\t{}.ca = {}.u8;", xer(), temp());
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ADDI:
                print("\t{}.s64 = ", r(insn.operands[0]));
                if (insn.operands[1] != 0)
                    print("{}.s64 + ", r(insn.operands[1]));
                println("{};", int32_t(insn.operands[2]));
                break;


            case PPC_INST_ADDIC:
                println("\t{}.ca = {}.u32 > {};", xer(), r(insn.operands[1]), ~insn.operands[2]);
                println("\t{}.s64 = {}.s64 + {};", r(insn.operands[0]), r(insn.operands[1]), int32_t(insn.operands[2]));
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ADDIS:
                print("\t{}.s64 = ", r(insn.operands[0]));
                if (insn.operands[1] != 0)
                    print("{}.s64 + ", r(insn.operands[1]));
                println("{};", static_cast<int32_t>(insn.operands[2] << 16));
                break;


            case PPC_INST_ADDME:
                println("\t{}.u64 = {}.u64 + {}.ca - 1;", temp(), r(insn.operands[1]), xer());
                println("\t{}.ca = ({}.u64 > {}.u64) || ({}.u64 == {}.u64 && {}.ca);", xer(),
                    r(insn.operands[1]), temp(), r(insn.operands[1]), temp(), xer());
                println("\t{}.u64 = {}.u64;", r(insn.operands[0]), temp());
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});",
                        cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ADDZE:
                println("\t{}.s64 = {}.s64 + {}.ca;", temp(), r(insn.operands[1]), xer());
                println("\t{}.ca = {}.u32 < {}.u32;", xer(), temp(), r(insn.operands[1]));
                println("\t{}.s64 = {}.s64;", r(insn.operands[0]), temp());
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_AND:
                println("\t{}.u64 = {}.u64 & {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ANDC:
                println("\t{}.u64 = {}.u64 & ~{}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
                if (strchr(insn.opcode->name, '.'))
                    println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ANDI:
                println("\t{}.u64 = {}.u64 & {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
                println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ANDIS:
                println("\t{}.u64 = {}.u64 & {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2] << 16);
                println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
                break;


            case PPC_INST_ATTN:
                // undefined instruction
                break;


            case PPC_INST_B:
                if (insn.operands[0] < fn.base || insn.operands[0] >= fn.base + fn.size)
                {
                    printFunctionCall(insn.operands[0]);
                    println("\tPPC_RETURN();");
                }
                else
                {
                    println("\tgoto loc_{:X};", insn.operands[0]);
                }
                break;
            


    case PPC_INST_BCTR:
        if (switchTable != config.switchTables.end())
        {
            // s13 (NFS MW race-load crash): switch on the LOW 32 BITS.
            //
            // The bounds check that guards a jump table is a 32-bit compare
            // (cmplwi), so the guest only ever guarantees the low half of the
            // register is in range -- PowerPC leaves the upper half of a
            // register holding a 32-bit value as whatever the caller left
            // there. Switching on .u64 means a dirty upper half matches no
            // case, falls into "default: __builtin_unreachable()", and hands
            // the optimiser a licence to emit an unguarded scaled load.
            //
            // Observed exactly that in sub_8217F2D8: r4.u64 = 0x100000000 with
            // low half 0 (a legitimate index 0), clang folded the four cases
            // into base + 0x82A30948 + 4*rax using the full 64-bit rax, and the
            // read landed 0x400000000 past the guest image. Note the indirect
            // CALL path below already uses .u32 -- this was the odd one out.
            println("\tswitch ({}.u32) {{", r(switchTable->second.r));

            for (size_t i = 0; i < switchTable->second.labels.size(); i++)
            {
                println("\tcase {}:", i);
                auto label = switchTable->second.labels[i];
                if (label < fn.base || label >= fn.base + fn.size)
                {
                    println("\t\t// ERROR: 0x{:X}", label);
                    fmt::println("ERROR: Switch case at {:X} is trying to jump outside function: {:X}", base, label);
                    println("\t\tPPC_RETURN();");
                }
                else
                {
                    println("\t\tgoto loc_{:X};", label);
                }
            }

            // s4: a LIVE default arm. `__builtin_unreachable()` here promised
            // the optimiser the index is always in range, and clang cashed that
            // in by deleting the bounds check -- the s3 Fable II crash was an
            // unguarded `movsxd rax,[rcx+rax*4]` in sub_822D8180 after
            // switch.toml recorded four of the guest's five labels.
            //
            // ctr already holds the address real hardware would take (every
            // instruction feeding it is translated faithfully; only the bctr is
            // special-cased), so the fallback is both safe and correct. See
            // PPC_SWITCH_MISS in ppc_context.h, which is a no-op unless the
            // build opts into logging.
            println("\tdefault:");
            println("\t\tPPC_SWITCH_MISS(0x{:X}, {}.u32, {}.u32);", base,
                r(switchTable->second.r), ctr());
            println("\t\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctr());
            println("\t\tPPC_RETURN();");
            println("\t}}");

            switchTable = config.switchTables.end();
        }
        else
        {
            println("\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctr());
            println("\tPPC_RETURN();");
        }
        break;



    case PPC_INST_BCTRL:
        if (!config.skipLr)
            println("\tADJ(ctx)->lr = 0x{:X};", base + 4);
        println("\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctr());
        csrState = CSRState::Unknown; // the call could change it
        break;


    case PPC_INST_BDZ:
        // Same out-of-range handling as BDNZ: a target outside the function is
        // a tail call, not a label. Without this the emitted `goto` names a
        // label that does not exist once the target is its own function.
        println("\t--{}.u64;", ctr());
        if (insn.operands[0] < fn.base || insn.operands[0] >= fn.base + fn.size)
        {
            println("\tif ({}.u32 == 0) {{", ctr());
            print("\t");
            printFunctionCall(insn.operands[0]);
            println("\t\tPPC_RETURN();");
            println("\t}}");
        }
        else
        {
            println("\tif ({}.u32 == 0) goto loc_{:X};", ctr(), insn.operands[0]);
        }
        break;


    case PPC_INST_BDZLR:
        println("\t--{}.u64;", ctr());
        println("\tif ({}.u32 == 0) PPC_RETURN();", ctr(), insn.operands[0]);
        break;


    case PPC_INST_BDNZ:
        println("\t--{}.u64;", ctr());
        if (insn.operands[0] < fn.base || insn.operands[0] >= fn.base + fn.size)
        {
            println("\tif ({}.u32 != 0) {{", ctr());
            print("\t");
            printFunctionCall(insn.operands[0]);
            println("\t\tPPC_RETURN();");
            println("\t}}");
        }
        else
        {
            println("\tif ({}.u32 != 0) goto loc_{:X};", ctr(), insn.operands[0]);
        }
        break;


    case PPC_INST_BDNZF:
        // NOTE: assuming eq here as a shortcut because all the instructions in the game do that
        println("\t--{}.u64;", ctr());
        if (insn.operands[1] < fn.base || insn.operands[1] >= fn.base + fn.size)
        {
            println("\tif ({}.u32 != 0 && !{}.eq) {{", ctr(), cr(insn.operands[0] / 4));
            print("\t");
            printFunctionCall(insn.operands[1]);
            println("\t\tPPC_RETURN();");
            println("\t}}");
        }
        else
        {
            println("\tif ({}.u32 != 0 && !{}.eq) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), insn.operands[1]);
        }
        break;


    case PPC_INST_BDNZT:
        // NOTE(crack): Same note as BDNZF but true instead of false
        println("\t--{}.u64;", ctr());
        if (insn.operands[1] < fn.base || insn.operands[1] >= fn.base + fn.size)
        {
            println("\tif ({}.u32 != 0 && {}.eq) {{", ctr(), cr(insn.operands[0] / 4));
            print("\t");
            printFunctionCall(insn.operands[1]);
            println("\t\tPPC_RETURN();");
            println("\t}}");
        }
        else
        {
            println("\tif ({}.u32 != 0 && {}.eq) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), insn.operands[1]);
        }
        break;


    case PPC_INST_BEQ:
        printConditionalBranch(false, "eq");
        break;


    case PPC_INST_BEQLR:
        println("\tif ({}.eq) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_BGE:
        printConditionalBranch(true, "lt");
        break;


    case PPC_INST_BGELR:
        println("\tif (!{}.lt) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_BGT:
        printConditionalBranch(false, "gt");
        break;


    case PPC_INST_BGTLR:
        println("\tif ({}.gt) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_BL:
        if (!config.skipLr)
            println("\tADJ(ctx)->lr = 0x{:X};", base + 4);
        printFunctionCall(insn.operands[0]);
        csrState = CSRState::Unknown; // the call could change it
        break;


    case PPC_INST_BLE:
        printConditionalBranch(true, "gt");
        break;


    case PPC_INST_BLELR:
        println("\tif (!{}.gt) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_BLR:
        println("\tPPC_RETURN();");
        break;


    case PPC_INST_BLRL:
        println("__builtin_debugtrap();");
        break;


    case PPC_INST_BLT:
        printConditionalBranch(false, "lt");
        break;


    case PPC_INST_BLTLR:
        println("\tif ({}.lt) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_BNE:
        printConditionalBranch(true, "eq");
        break;


    case PPC_INST_BNECTR:
        println("\tif (!{}.eq) {{", cr(insn.operands[0]));
        println("\t\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctr());
        println("\t\tPPC_RETURN();");
        println("\t}}");
        break;


    case PPC_INST_BNELR:
        println("\tif (!{}.eq) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_CCTPL:
        // no op
        break;


    case PPC_INST_CCTPM:
        // no op
        break;


    case PPC_INST_CLRLDI:
        println("\t{}.u64 = {}.u64 & 0x{:X};", r(insn.operands[0]), r(insn.operands[1]), (1ull << (64 - insn.operands[2])) - 1);
        break;


    case PPC_INST_CLRLWI:
        println("\t{}.u64 = {}.u32 & 0x{:X};", r(insn.operands[0]), r(insn.operands[1]), (1ull << (32 - insn.operands[2])) - 1);
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_CMPD:
        println("\t{}.compare<int64_t>({}.s64, {}.s64, {});", cr(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]), xer());
        break;


    case PPC_INST_CMPDI:
        println("\t{}.compare<int64_t>({}.s64, {}, {});", cr(insn.operands[0]), r(insn.operands[1]), int32_t(insn.operands[2]), xer());
        break;


    case PPC_INST_CMPLD:
        println("\t{}.compare<uint64_t>({}.u64, {}.u64, {});", cr(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]), xer());
        break;


    case PPC_INST_CMPLDI:
        println("\t{}.compare<uint64_t>({}.u64, {}, {});", cr(insn.operands[0]), r(insn.operands[1]), insn.operands[2], xer());
        break;


    case PPC_INST_CMPLW:
        println("\t{}.compare<uint32_t>({}.u32, {}.u32, {});", cr(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]), xer());
        break;


    case PPC_INST_CMPLWI:
        println("\t{}.compare<uint32_t>({}.u32, {}, {});", cr(insn.operands[0]), r(insn.operands[1]), insn.operands[2], xer());
        break;


    case PPC_INST_CMPW:
        println("\t{}.compare<int32_t>({}.s32, {}.s32, {});", cr(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]), xer());
        break;


    case PPC_INST_CMPWI:
        println("\t{}.compare<int32_t>({}.s32, {}, {});", cr(insn.operands[0]), r(insn.operands[1]), int32_t(insn.operands[2]), xer());
        break;


    case PPC_INST_CNTLZD:
        println("\t{0}.u64 = {1}.u64 == 0 ? 64 : __builtin_clzll({1}.u64);", r(insn.operands[0]), r(insn.operands[1]));
        break;


    case PPC_INST_CNTLZW:
        println("\t{0}.u64 = {1}.u32 == 0 ? 32 : __builtin_clz({1}.u32);", r(insn.operands[0]), r(insn.operands[1]));
        break;


    case PPC_INST_DB16CYC:
        // no op
        break;


    case PPC_INST_DCBF:
        // no op
        break;


    case PPC_INST_DCBT:
        // no op
        break;


    case PPC_INST_DCBST:
    case PPC_INST_DCBTST:
        // no op
        break;


    case PPC_INST_DCBZ:
        print("\tmemset((void*)(base + ((");
        if (insn.operands[0] != 0)
            print("{}.u32 + ", r(insn.operands[0]));
        println("{}.u32) & ~31)), 0, 32);", r(insn.operands[1]));
        break;


    case PPC_INST_DCBZL:
        print("\tmemset((void*)(base + ((");
        if (insn.operands[0] != 0)
            print("{}.u32 + ", r(insn.operands[0]));
        println("{}.u32) & ~127)), 0, 128);", r(insn.operands[1]));
        break;


    case PPC_INST_DIVD:
        println("\t{}.s64 = {}.s64 / {}.s64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_DIVDU:
        println("\t{}.u64 = {}.u64 / {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_DIVW:
        println("\t{}.s32 = {}.s32 / {}.s32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_DIVWU:
        println("\t{}.u32 = {}.u32 / {}.u32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_EIEIO:
        // no op
        break;


    case PPC_INST_EQV:
        // rA = ~(rS XOR rB)
        println("\t{}.u64 = ~({}.u64 ^ {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_EXTSB:
        println("\t{}.s64 = {}.s8;", r(insn.operands[0]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_EXTSH:
        println("\t{}.s64 = {}.s16;", r(insn.operands[0]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_EXTSW:
        println("\t{}.s64 = {}.s32;", r(insn.operands[0]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_FABS:
        printSetFlushMode(false);
        println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FADD:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 + {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FADDS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64 + {}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FCFID:
        printSetFlushMode(false);
        println("\t{}.f64 = double({}.s64);", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FCMPU:
        printSetFlushMode(false);
        println("\t{}.compare({}.f64, {}.f64);", cr(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FCTID:
        printSetFlushMode(false);
        println("\t{}.s64 = ({}.f64 > double(LLONG_MAX)) ? LLONG_MAX : _mm_cvtsd_si64(_mm_load_sd(&{}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[1]));
        break;


    case PPC_INST_FCTIDZ:
        printSetFlushMode(false);
        println("\t{}.s64 = ({}.f64 > double(LLONG_MAX)) ? LLONG_MAX : _mm_cvttsd_si64(_mm_load_sd(&{}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[1]));
        break;


    case PPC_INST_FCTIWZ:
        printSetFlushMode(false);
        println("\t{}.s64 = ({}.f64 > double(INT_MAX)) ? INT_MAX : _mm_cvttsd_si32(_mm_load_sd(&{}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[1]));
        break;


    case PPC_INST_FDIV:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 / {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FDIVS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64 / {}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FMADD:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 * {}.f64 + {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FMADDS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64 * {}.f64 + {}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FMR:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64;", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FMSUB:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 * {}.f64 - {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FMSUBS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64 * {}.f64 - {}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FMUL:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 * {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FMULS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64 * {}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FNABS:
        printSetFlushMode(false);
        println("\t{}.u64 = {}.u64 | 0x8000000000000000;", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FNEG:
        printSetFlushMode(false);
        println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FNMADDS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float(-({}.f64 * {}.f64 + {}.f64)));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FNMSUB:
        printSetFlushMode(false);
        println("\t{}.f64 = -({}.f64 * {}.f64 - {}.f64);", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FNMSUBS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float(-({}.f64 * {}.f64 - {}.f64)));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FRES:
        printSetFlushMode(false);
        println("\t{}.f64 = float(1.0 / {}.f64);", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FRSP:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64));", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FSEL:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;


    case PPC_INST_FRSQRTE:
        // TODO(crack): I sure hope the c++ optimizer can optimize this. Fixme with some simd magic later
        printSetFlushMode(false);
        println("\t{}.f64 = double(1.0f / sqrtf(float({}.f64)));", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FSQRT:
        printSetFlushMode(false);
        println("\t{}.f64 = sqrt({}.f64);", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FSQRTS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float(sqrt({}.f64)));", f(insn.operands[0]), f(insn.operands[1]));
        break;


    case PPC_INST_FSUB:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 - {}.f64;", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_FSUBS:
        printSetFlushMode(false);
        println("\t{}.f64 = double(float({}.f64 - {}.f64));", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]));
        break;


    case PPC_INST_LBZ:
        print("\t{}.u64 = PPC_LOAD_U8(", r(insn.operands[0]));
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{});", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LBZU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U8({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LBZX:
        print("\t{}.u64 = PPC_LOAD_U8(", r(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        break;


    case PPC_INST_LD:
        print("\t{}.u64 = PPC_LOAD_U64(", r(insn.operands[0]));
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{});", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LDARX:
        print("\t{}.u64 = *(uint64_t*)(base + ", reserved());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        println("\t{}.u64 = __builtin_bswap64({}.u64);", r(insn.operands[0]), reserved());
        break;


    case PPC_INST_LDU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U64({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LDX:
        print("\t{}.u64 = PPC_LOAD_U64(", r(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        break;


    case PPC_INST_LFD:
        printSetFlushMode(false);
        print("\t{}.u64 = PPC_LOAD_U64(", f(insn.operands[0]));
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{});", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LFDX:
        printSetFlushMode(false);
        print("\t{}.u64 = PPC_LOAD_U64(", f(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        break;


    case PPC_INST_LFS:
        printSetFlushMode(false);
        print("\t{}.u32 = PPC_LOAD_U32(", temp());
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{});", int32_t(insn.operands[1]));
        println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
        break;


    case PPC_INST_LFSX:
        printSetFlushMode(false);
        print("\t{}.u32 = PPC_LOAD_U32(", temp());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
        break;


    case PPC_INST_LHA:
        print("\t{}.s64 = int16_t(PPC_LOAD_U16(", r(insn.operands[0]));
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}));", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LHAX:
        print("\t{}.s64 = int16_t(PPC_LOAD_U16(", r(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32));", r(insn.operands[2]));
        break;


    case PPC_INST_LHZ:
        print("\t{}.u64 = PPC_LOAD_U16(", r(insn.operands[0]));
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{});", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LHZX:
        print("\t{}.u64 = PPC_LOAD_U16(", r(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        break;


    case PPC_INST_LI:
        println("\t{}.s64 = {};", r(insn.operands[0]), int32_t(insn.operands[1]));
        break;


    case PPC_INST_LIS:
        println("\t{}.s64 = {};", r(insn.operands[0]), int32_t(insn.operands[1] << 16));
        break;


  

    case PPC_INST_LVLX:
    case PPC_INST_LVLX128:
        print("\t{}.u32 = ", temp());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32;", r(insn.operands[2]));
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_shuffle_epi8(_mm_load_si128((__m128i*)(base + ({}.u32 & ~0xF))), _mm_load_si128((__m128i*)&VectorMaskL[({}.u32 & 0xF) * 16])));", v(insn.operands[0]), temp(), temp());
        break;


    case PPC_INST_LVRX:
    case PPC_INST_LVRX128:
        print("\t{}.u32 = ", temp());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32;", r(insn.operands[2]));
        println("\t_mm_store_si128((__m128i*){}.u8, {}.u32 & 0xF ? _mm_shuffle_epi8(_mm_load_si128((__m128i*)(base + ({}.u32 & ~0xF))), _mm_load_si128((__m128i*)&VectorMaskR[({}.u32 & 0xF) * 16])) : _mm_setzero_si128());", v(insn.operands[0]), temp(), temp(), temp());
        break;


    case PPC_INST_LVSL:
        print("\t{}.u32 = ", temp());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32;", r(insn.operands[2]));
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_load_si128((__m128i*)&VectorShiftTableL[({}.u32 & 0xF) * 16]));", v(insn.operands[0]), temp());
        break;


    case PPC_INST_LVSR:
        print("\t{}.u32 = ", temp());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32;", r(insn.operands[2]));
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_load_si128((__m128i*)&VectorShiftTableR[({}.u32 & 0xF) * 16]));", v(insn.operands[0]), temp());
        break;


    case PPC_INST_LWA:
        print("\t{}.s64 = int32_t({}(", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}));", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LWARX:
        print("\t{}.u32 = *(uint32_t*)(base + ", reserved());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        println("\t{}.u64 = __builtin_bswap32({}.u32);", r(insn.operands[0]), reserved());
        break;


    case PPC_INST_LWAX:
        print("\t{}.s64 = int32_t({}(", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32));", r(insn.operands[2]));
        break;


    case PPC_INST_LWBRX:
        print("\t{}.u64 = __builtin_bswap32({}(", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32));", r(insn.operands[2]));
        break;


    case PPC_INST_LWSYNC:
        // no op
        break;


    case PPC_INST_LWZ:
        print("\t{}.u64 = {}(", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{});", int32_t(insn.operands[1]));
        break;


    case PPC_INST_LWZU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = {}({});", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32", ea());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LWZX:
        print("\t{}.u64 = {}(", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        break;

    case PPC_INST_LWAUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.s64 = int32_t({}({}));", r(insn.operands[0]), mmioStore() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32", ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;

    case PPC_INST_MFCR:
        for (size_t i = 0; i < 32; i++)
        {
            constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
            println("\t{}.u64 {}= {}.{} ? 0x{:X} : 0;", r(insn.operands[0]), i == 0 ? "" : "|", cr(i / 4), fields[i % 4], 1u << (31 - i));
        }
        break;



    case PPC_INST_MFFS:
        println("\t{}.u64 = ADJ(ctx)->fpscr.loadFromHost();", f(insn.operands[0]));
        break;


    case PPC_INST_MFLR:
        if (!config.skipLr)
            println("\t{}.u64 = ADJ(ctx)->lr;", r(insn.operands[0]));
        break;


    case PPC_INST_MFMSR:
        if (!config.skipMsr)
            println("\t{}.u64 = ADJ(ctx)->msr;", r(insn.operands[0]));
        break;


    case PPC_INST_MFOCRF:
        // TODO: don't hardcode to cr6
        println("\t{}.u64 = ({}.lt << 7) | ({}.gt << 6) | ({}.eq << 5) | ({}.so << 4);", r(insn.operands[0]), cr(6), cr(6), cr(6), cr(6));
        break;


    case PPC_INST_MFTB:
        // s23: the 360 timebase is 50 MHz and the guest kernel reports
        // KeQueryPerformanceFrequency = 50,000,000, so a raw host TSC here is
        // ~70-100x too fast (NFS MW's frame timer saw ~1.3 s per frame and
        // pinned its sim-step clamp every frame). zz_mftb() (work/ppc/
        // zz_time.cpp) returns a 50 MHz timebase; it is declared in
        // ppc_context.h.
        println("\t{}.u64 = zz_mftb();", r(insn.operands[0]));
        break;


    case PPC_INST_MR:
        println("\t{}.u64 = {}.u64;", r(insn.operands[0]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_MTCR:
        for (size_t i = 0; i < 32; i++)
        {
            constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
            println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;", cr(i / 4), fields[i % 4], r(insn.operands[0]), 1u << (31 - i));
        }
        break;



    case PPC_INST_MTCTR:
        println("\t{}.u64 = {}.u64;", ctr(), r(insn.operands[0]));
        break;


    case PPC_INST_MTFSF:
        println("\tADJ(ctx)->fpscr.storeFromGuest({}.u32);", f(insn.operands[1]));
        break;


    case PPC_INST_MTLR:
        if (!config.skipLr)
            println("\tADJ(ctx)->lr = {}.u64;", r(insn.operands[0]));
        break;


    case PPC_INST_MTMSRD:
        if (!config.skipMsr)
            println("\tADJ(ctx)->msr = ({}.u32 & 0x8020) | (ADJ(ctx)->msr & ~0x8020);", r(insn.operands[0]));
        break;


    case PPC_INST_MTXER:
        println("\t{}.so = ({}.u64 & 0x80000000) != 0;", xer(), r(insn.operands[0]));
        println("\t{}.ov = ({}.u64 & 0x40000000) != 0;", xer(), r(insn.operands[0]));
        println("\t{}.ca = ({}.u64 & 0x20000000) != 0;", xer(), r(insn.operands[0]));
        break;


    case PPC_INST_MULHW:
        println("\t{}.s64 = (int64_t({}.s32) * int64_t({}.s32)) >> 32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_MULHWU:
        println("\t{}.u64 = (uint64_t({}.u32) * uint64_t({}.u32)) >> 32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_MULLD:
        println("\t{}.s64 = {}.s64 * {}.s64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_MULLI:
        println("\t{}.s64 = {}.s64 * {};", r(insn.operands[0]), r(insn.operands[1]), int32_t(insn.operands[2]));
        break;


    case PPC_INST_MULLW:
        println("\t{}.s64 = int64_t({}.s32) * int64_t({}.s32);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_MULHD:
        println("\t{}.s64 = __mulh({}.s64, {}.s64);",
            r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});",
                cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_MULHDU:
        println("\t{}.u64 = __mulhu({}.u64, {}.u64);",
            r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});",
                cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_NAND:
        println("\t{}.u64 = ~({}.u64 & {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_NEG:
        println("\t{}.s64 = -{}.s64;", r(insn.operands[0]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_NOP:
        // no op
        break;


    case PPC_INST_NOR:
        println("\t{}.u64 = ~({}.u64 | {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_NOT:
        println("\t{}.u64 = ~{}.u64;", r(insn.operands[0]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_OR:
        println("\t{}.u64 = {}.u64 | {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_ORC:
        println("\t{}.u64 = {}.u64 | ~{}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_ORI:
        println("\t{}.u64 = {}.u64 | {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
        break;


    case PPC_INST_ORIS:
        println("\t{}.u64 = {}.u64 | {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2] << 16);
        break;


    case PPC_INST_RLDICL:
        println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2], ComputeMask(insn.operands[3], 63));
        break;


    case PPC_INST_RLDICR:
        println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2], ComputeMask(0, insn.operands[3]));
        break;


    case PPC_INST_RLDIMI:
    {
        const uint64_t mask = ComputeMask(insn.operands[3], ~insn.operands[2]);
        println("\t{}.u64 = (__builtin_rotateleft64({}.u64, {}) & 0x{:X}) | ({}.u64 & 0x{:X});", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2], mask, r(insn.operands[0]), ~mask);
        break;
    }


    case PPC_INST_RLWIMI:
    {
        const uint64_t mask = ComputeMask(insn.operands[3] + 32, insn.operands[4] + 32);
        println("\t{}.u64 = (__builtin_rotateleft32({}.u32, {}) & 0x{:X}) | ({}.u64 & 0x{:X});", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2], mask, r(insn.operands[0]), ~mask);
        break;
    }


    case PPC_INST_RLWINM:
        println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}) & 0x{:X};", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[1]), insn.operands[2], ComputeMask(insn.operands[3] + 32, insn.operands[4] + 32));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_RLWNM:
        println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}.u8 & 0x1F) & 0x{:X};",
            r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[1]),
            r(insn.operands[2]), ComputeMask(insn.operands[3] + 32, insn.operands[4] + 32));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_ROTLDI:
        println("\t{}.u64 = __builtin_rotateleft64({}.u64, {});", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
        break;


    case PPC_INST_ROTLW:
        println("\t{}.u64 = __builtin_rotateleft32({}.u32, {}.u8 & 0x1F);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_ROTLWI:
        println("\t{}.u64 = __builtin_rotateleft32({}.u32, {});", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SLD:
        println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 << ({}.u8 & 0x7F));", r(insn.operands[0]), r(insn.operands[2]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_SLW:
        println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 << ({}.u8 & 0x3F));", r(insn.operands[0]), r(insn.operands[2]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SRAD:
        println("\t{}.u64 = {}.u64 & 0x7F;", temp(), r(insn.operands[2]));
        println("\tif ({}.u64 > 0x3F) {}.u64 = 0x3F;", temp(), temp());
        println("\t{}.ca = ({}.s64 < 0) & ((({}.s64 >> {}.u64) << {}.u64) != {}.s64);", xer(), r(insn.operands[1]), r(insn.operands[1]), temp(), temp(), r(insn.operands[1]));
        println("\t{}.s64 = {}.s64 >> {}.u64;", r(insn.operands[0]), r(insn.operands[1]), temp());
        break;


    case PPC_INST_SRADI:
        if (insn.operands[2] != 0)
        {
            println("\t{}.ca = ({}.s64 < 0) & (({}.u64 & 0x{:X}) != 0);", xer(), r(insn.operands[1]), r(insn.operands[1]), ComputeMask(64 - insn.operands[2], 63));
            println("\t{}.s64 = {}.s64 >> {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
        }
        else
        {
            println("\t{}.ca = 0;", xer());
            println("\t{}.s64 = {}.s64;", r(insn.operands[0]), r(insn.operands[1]));
        }
        break;



    case PPC_INST_SRAW:
        println("\t{}.u32 = {}.u32 & 0x3F;", temp(), r(insn.operands[2]));
        println("\tif ({}.u32 > 0x1F) {}.u32 = 0x1F;", temp(), temp());
        println("\t{}.ca = ({}.s32 < 0) & ((({}.s32 >> {}.u32) << {}.u32) != {}.s32);", xer(), r(insn.operands[1]), r(insn.operands[1]), temp(), temp(), r(insn.operands[1]));
        println("\t{}.s64 = {}.s32 >> {}.u32;", r(insn.operands[0]), r(insn.operands[1]), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SRAWI:
        if (insn.operands[2] != 0)
        {
            println("\t{}.ca = ({}.s32 < 0) & (({}.u32 & 0x{:X}) != 0);", xer(), r(insn.operands[1]), r(insn.operands[1]), ComputeMask(64 - insn.operands[2], 63));
            println("\t{}.s64 = {}.s32 >> {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
        }
        else
        {
            println("\t{}.ca = 0;", xer());
            println("\t{}.s64 = {}.s32;", r(insn.operands[0]), r(insn.operands[1]));
        }
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;



    case PPC_INST_SRD:
        println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 >> ({}.u8 & 0x7F));", r(insn.operands[0]), r(insn.operands[2]), r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_SRW:
        println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 >> ({}.u8 & 0x3F));", r(insn.operands[0]), r(insn.operands[2]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_STB:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U8(" : "\tPPC_STORE_U8(");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}, {}.u8);", int32_t(insn.operands[1]), r(insn.operands[0]));
        break;


    case PPC_INST_STBU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\tPPC_STORE_U8({}, {}.u8);", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_STBX:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U8(" : "\tPPC_STORE_U8(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u8);", r(insn.operands[2]), r(insn.operands[0]));
        break;


    case PPC_INST_STD:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U64(" : "\tPPC_STORE_U64(");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}, {}.u64);", int32_t(insn.operands[1]), r(insn.operands[0]));
        break;


    case PPC_INST_STDCX:
        println("\t{}.lt = 0;", cr(0));
        println("\t{}.gt = 0;", cr(0));
        print("\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + ", cr(0));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32), {}.s64, __builtin_bswap64({}.s64));", r(insn.operands[2]), reserved(), r(insn.operands[0]));
        println("\t{}.so = {}.so;", cr(0), xer());
        break;


    case PPC_INST_STDU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\tPPC_STORE_U64({}, {}.u64);", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_STDX:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U64(" : "\tPPC_STORE_U64(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u64);", r(insn.operands[2]), r(insn.operands[0]));
        break;


    case PPC_INST_STFD:
        printSetFlushMode(false);
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U64(" : "\tPPC_STORE_U64(");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}, {}.u64);", int32_t(insn.operands[1]), f(insn.operands[0]));
        break;


    case PPC_INST_STFDX:
        printSetFlushMode(false);
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U64(" : "\tPPC_STORE_U64(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u64);", r(insn.operands[2]), f(insn.operands[0]));
        break;


    case PPC_INST_STFIWX:
        printSetFlushMode(false);
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u32);", r(insn.operands[2]), f(insn.operands[0]));
        break;


    case PPC_INST_STFS:
        printSetFlushMode(false);
        println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}, {}.u32);", int32_t(insn.operands[1]), temp());
        break;


    case PPC_INST_STFSX:
        printSetFlushMode(false);
        println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u32);", r(insn.operands[2]), temp());
        break;


    case PPC_INST_STH:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U16(" : "\tPPC_STORE_U16(");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}, {}.u16);", int32_t(insn.operands[1]), r(insn.operands[0]));
        break;


    case PPC_INST_STHBRX:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U16(" : "\tPPC_STORE_U16(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, __builtin_bswap16({}.u16));", r(insn.operands[2]), r(insn.operands[0]));
        break;


    case PPC_INST_STHX:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U16(" : "\tPPC_STORE_U16(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u16);", r(insn.operands[2]), r(insn.operands[0]));
        break;


    case PPC_INST_STVEHX:
        // TODO: vectorize
        // NOTE: accounting for the full vector reversal here
        print("\t{} = (", ea());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32) & ~0x1;", r(insn.operands[2]));
        println("\tPPC_STORE_U16(ea, {}.u16[7 - (({} & 0xF) >> 1)]);", v(insn.operands[0]), ea());
        break;


    case PPC_INST_STVEWX:
    case PPC_INST_STVEWX128:
        // TODO: vectorize
        // NOTE: accounting for the full vector reversal here
        print("\t{} = (", ea());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32) & ~0x3;", r(insn.operands[2]));
        println("\tPPC_STORE_U32(ea, {}.u32[3 - (({} & 0xF) >> 2)]);", v(insn.operands[0]), ea());
        break;


    case PPC_INST_STVLX:
    case PPC_INST_STVLX128:
    case PPC_INST_STVLXL:
    case PPC_INST_STVLXL128:
        // TODO: vectorize
        // NOTE: accounting for the full vector reversal here
        print("\t{} = ", ea());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32;", r(insn.operands[2]));

        println("\tfor (size_t i = 0; i < (16 - ({} & 0xF)); i++)", ea());
        println("\t\tPPC_STORE_U8({} + i, {}.u8[15 - i]);", ea(), v(insn.operands[0]));
        break;


    case PPC_INST_STVRX:
    case PPC_INST_STVRX128:
    case PPC_INST_STVRXL:
    case PPC_INST_STVRXL128:
        // TODO: vectorize
        // NOTE: accounting for the full vector reversal here
        print("\t{} = ", ea());
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32;", r(insn.operands[2]));

        println("\tfor (size_t i = 0; i < ({} & 0xF); i++)", ea());
        println("\t\tPPC_STORE_U8({} - i - 1, {}.u8[i]);", ea(), v(insn.operands[0]));
        break;


    case PPC_INST_STVX:
    case PPC_INST_STVX128:
    case PPC_INST_STVXL:
    case PPC_INST_STVXL128:
        print("\t_mm_store_si128((__m128i*)(base + ((");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32) & ~0xF)), _mm_shuffle_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*)VectorMaskL)));", r(insn.operands[2]), v(insn.operands[0]));
        break;


    case PPC_INST_STW:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
        if (insn.operands[2] != 0)
            print("{}.u32 + ", r(insn.operands[2]));
        println("{}, {}.u32);", int32_t(insn.operands[1]), r(insn.operands[0]));
        break;


    case PPC_INST_STWBRX:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, __builtin_bswap32({}.u32));", r(insn.operands[2]), r(insn.operands[0]));
        break;


    case PPC_INST_STWCX:
        println("\t{}.lt = 0;", cr(0));
        println("\t{}.gt = 0;", cr(0));
        print("\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ", cr(0));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32), {}.s32, __builtin_bswap32({}.s32));", r(insn.operands[2]), reserved(), r(insn.operands[0]));
        println("\t{}.so = {}.so;", cr(0), xer());
        break;


    case PPC_INST_STWU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\tPPC_STORE_U32({}, {}.u32);", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_STWUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\tPPC_STORE_U32({}, {}.u32);", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_STWX:
        print("{}", mmioStore() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u32);", r(insn.operands[2]), r(insn.operands[0]));
        break;


    case PPC_INST_SUBF:
        println("\t{}.s64 = {}.s64 - {}.s64;", r(insn.operands[0]), r(insn.operands[2]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SUBFC:
        println("\t{}.ca = {}.u32 >= {}.u32;", xer(), r(insn.operands[2]), r(insn.operands[1]));
        println("\t{}.s64 = {}.s64 - {}.s64;", r(insn.operands[0]), r(insn.operands[2]), r(insn.operands[1]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SUBFE:
        println("\t{}.u8 = (~{}.u32 + {}.u32 < ~{}.u32) | (~{}.u32 + {}.u32 + {}.ca < {}.ca);", temp(), r(insn.operands[1]), r(insn.operands[2]), r(insn.operands[1]), r(insn.operands[1]), r(insn.operands[2]), xer(), xer());
        println("\t{}.u64 = ~{}.u64 + {}.u64 + {}.ca;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SUBFIC:
        println("\t{}.ca = {}.u32 <= {};", xer(), r(insn.operands[1]), insn.operands[2]);
        println("\t{}.s64 = {} - {}.s64;", r(insn.operands[0]), int32_t(insn.operands[2]), r(insn.operands[1]));
        break;


    case PPC_INST_SUBFME:
        println("\t{}.u64 = ~{}.u64 + {}.ca - 1;", temp(), r(insn.operands[1]), xer());
        println("\t{}.ca = ({}.u64 < ~{}.u64) || ({}.u64 == ~{}.u64 && {}.ca);", xer(),
            temp(), r(insn.operands[1]), temp(), r(insn.operands[1]), xer());
        println("\t{}.u64 = {}.u64;", r(insn.operands[0]), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});",
                cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SUBFZE:
        println("\t{}.u64 = ~{}.u64 + {}.ca;", temp(), r(insn.operands[1]), xer());
        println("\t{}.ca = {}.u64 < {}.ca;", xer(), temp(), xer());
        println("\t{}.u64 = {}.u64;", r(insn.operands[0]), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_SYNC:
        // no op
        break;


    case PPC_INST_TDEQ:
        println("\tif ({}.u64 == {}.u64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDEQI:
        println("\tif ({}.u64 == {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TDGE:
        println("\tif ({}.s64 >= {}.s64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDGEI:
        println("\tif ({}.s64 >= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TDGT:
        println("\tif ({}.s64 > {}.s64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDGTI:
        println("\tif ({}.s64 > {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TDLE:
        println("\tif ({}.s64 <= {}.s64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDLEI:
        println("\tif ({}.s64 <= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TDLGE:
        println("\tif ({}.u64 >= {}.u64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDLGEI:
        println("\tif ({}.u64 >= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TDLGT:
        println("\tif ({}.u64 > {}.u64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDLGTI:
        println("\tif ({}.u64 > {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TDLLE:
        println("\tif ({}.u64 <= {}.u64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDLLEI:
        println("\tif ({}.u64 <= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TDLLT:
        println("\tif ({}.u64 < {}.u64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDLLTI:
        println("\tif ({}.u64 < {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TDLT:
        println("\tif ({}.s64 < {}.s64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDLTI:
        println("\tif ({}.s64 < {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TDNE:
        println("\tif ({}.u64 != {}.u64) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TDNEI:
        println("\tif ({}.u64 != {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TWI:
    {        // TO field specifies trap conditions:
        // Bit 0 (16): Less than (signed)
        // Bit 1 (8): Greater than (signed)  
        // Bit 2 (4): Equal
        // Bit 3 (2): Less than (unsigned)
        // Bit 4 (1): Greater than (unsigned)

        bool first = true;
        print("\tif (");

        if (insn.operands[0] & 16) {
            print("{}.s32 < {}", r(insn.operands[1]), int32_t(insn.operands[2]));
            first = false;
        }

        if (insn.operands[0] & 8) {
            if (!first) print(" || ");
            print("{}.s32 > {}", r(insn.operands[1]), int32_t(insn.operands[2]));
            first = false;
        }

        if (insn.operands[0] & 4) {
            if (!first) print(" || ");
            print("{}.u32 == {}", r(insn.operands[1]), insn.operands[2]);
            first = false;
        }

        if (insn.operands[0] & 2) {
            if (!first) print(" || ");
            print("{}.u32 < {}", r(insn.operands[1]), insn.operands[2]);
            first = false;
        }

        if (insn.operands[0] & 1) {
            if (!first) print(" || ");
            print("{}.u32 > {}", r(insn.operands[1]), insn.operands[2]);
            first = false;
        }

        if (first) {
            // TO = 0 means never trap
            println("false) PPC_TRAP(0x{:X});", base);
        }
        else {
            println(") PPC_TRAP(0x{:X});", base);
        }
    }
    break;



    case PPC_INST_TWEQ:
        println("\tif ({}.u32 == {}.u32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWEQI:
        println("\tif ({}.u32 == {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TWGE:
        println("\tif ({}.s32 >= {}.s32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWGEI:
        println("\tif ({}.s32 >= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TWGT:
        println("\tif ({}.s32 > {}.s32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWGTI:
        println("\tif ({}.s32 > {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TWLE:
        println("\tif ({}.s32 <= {}.s32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWLEI:
        println("\tif ({}.s32 <= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TWLGE:
        println("\tif ({}.u32 >= {}.u32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWLGEI:
        println("\tif ({}.u32 >= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TWLGT:
        println("\tif ({}.u32 > {}.u32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWLGTI:
        println("\tif ({}.u32 > {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TWLLE:
        println("\tif ({}.u32 <= {}.u32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWLLEI:
        println("\tif ({}.u32 <= {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TWLLT:
        println("\tif ({}.u32 < {}.u32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWLLTI:
        println("\tif ({}.u32 < {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_TWLT:
        println("\tif ({}.s32 < {}.s32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWLTI:
        println("\tif ({}.s32 < {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), int32_t(insn.operands[1]), base);
        break;


    case PPC_INST_TWNE:
        println("\tif ({}.u32 != {}.u32) PPC_TRAP(0x{:X});", r(insn.operands[0]), r(insn.operands[1]), base);
        break;


    case PPC_INST_TWNEI:
        println("\tif ({}.u32 != {}) PPC_TRAP(0x{:X});", r(insn.operands[0]), insn.operands[1], base);
        break;


    case PPC_INST_VADDFP:
    case PPC_INST_VADDFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_add_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDSHS:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_adds_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VADDUBM:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_add_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDUBS:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_adds_epu8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDUHM:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_add_epi16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDUWM:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_add_epi32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDUWS:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_adds_epu32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VAND:
    case PPC_INST_VAND128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_and_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VANDC:
    case PPC_INST_VANDC128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_andnot_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VAVGSB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_avg_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VAVGSH:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_avg_epi16(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VAVGUB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_avg_epu8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VAVGUH:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_avg_epu16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VCTSXS:
    case PPC_INST_VCFPSXWS128:
        printSetFlushMode(true);
        print("\t_mm_store_si128((__m128i*){}.s32, _mm_vctsxs(", v(insn.operands[0]));
        if (insn.operands[2] != 0)
            println("_mm_mul_ps(_mm_load_ps({}.f32), _mm_set1_ps({}))));", v(insn.operands[1]), 1u << insn.operands[2]);
        else
            println("_mm_load_ps({}.f32)));", v(insn.operands[1]));
        break;


    case PPC_INST_VCTUXS:
    case PPC_INST_VCFPUXWS128:
        printSetFlushMode(true);
        print("\t_mm_store_si128((__m128i*){}.u32, _mm_vctuxs(", v(insn.operands[0]));
        if (insn.operands[2] != 0)
            println("_mm_mul_ps(_mm_load_ps({}.f32), _mm_set1_ps({}))));", v(insn.operands[1]), 1u << insn.operands[2]);
        else
            println("_mm_load_ps({}.f32)));", v(insn.operands[1]));
        break;


    case PPC_INST_VCFSX:
    case PPC_INST_VCSXWFP128:
    {
        printSetFlushMode(true);
        print("\t_mm_store_ps({}.f32, ", v(insn.operands[0]));
        if (insn.operands[2] != 0)
        {
            const float value = ldexp(1.0f, -int32_t(insn.operands[2]));
            println("_mm_mul_ps(_mm_cvtepi32_ps(_mm_load_si128((__m128i*){}.u32)), _mm_castsi128_ps(_mm_set1_epi32(int(0x{:X})))));", v(insn.operands[1]), *reinterpret_cast<const uint32_t*>(&value));
        }
        else
        {
            println("_mm_cvtepi32_ps(_mm_load_si128((__m128i*){}.u32)));", v(insn.operands[1]));
        }
        break;
    }


    case PPC_INST_VCFUX:
    case PPC_INST_VCUXWFP128:
    {
        printSetFlushMode(true);
        print("\t_mm_store_ps({}.f32, ", v(insn.operands[0]));
        if (insn.operands[2] != 0)
        {
            const float value = ldexp(1.0f, -int32_t(insn.operands[2]));
            println("_mm_mul_ps(_mm_cvtepu32_ps_(_mm_load_si128((__m128i*){}.u32)), _mm_castsi128_ps(_mm_set1_epi32(int(0x{:X})))));", v(insn.operands[1]), *reinterpret_cast<const uint32_t*>(&value));
        }
        else
        {
            println("_mm_cvtepu32_ps_(_mm_load_si128((__m128i*){}.u32)));", v(insn.operands[1]));
        }
        break;
    }


    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_vcmpbfp(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPEQFP:
    case PPC_INST_VCMPEQFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_cmpeq_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPEQUB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_cmpeq_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_si128((__m128i*){}.u8), 0xFFFF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPEQUW:
    case PPC_INST_VCMPEQUW128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_cmpeq_epi32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPGEFP:
    case PPC_INST_VCMPGEFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_cmpge_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPGTFP:
    case PPC_INST_VCMPGTFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_cmpgt_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPGTUB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_cmpgt_epu8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VCMPGTUH:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_cmpgt_epu16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VEXPTEFP:
    case PPC_INST_VEXPTEFP128:
        // TODO: vectorize
        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
            println("\t{}.f32[{}] = exp2f({}.f32[{}]);", v(insn.operands[0]), i, v(insn.operands[1]), i);
        break;


    case PPC_INST_VLOGEFP:
    case PPC_INST_VLOGEFP128:
        // TODO: vectorize
        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
            println("\t{}.f32[{}] = log2f({}.f32[{}]);", v(insn.operands[0]), i, v(insn.operands[1]), i);
        break;


    case PPC_INST_VMADDCFP128:
    case PPC_INST_VMADDFP:
    case PPC_INST_VMADDFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_add_ps(_mm_mul_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]), v(insn.operands[3]));
        break;


    case PPC_INST_VMAXFP:
    case PPC_INST_VMAXFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_max_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMAXSH:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_max_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMAXSW:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_max_epi32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMAXUH:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_max_epu16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMINFP:
    case PPC_INST_VMINFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_min_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMINSH:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_min_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMINUH:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_min_epu16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMRGHB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_unpackhi_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VMRGHH:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_unpackhi_epi16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VMRGHW:
    case PPC_INST_VMRGHW128:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_unpackhi_epi32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VMRGLB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_unpacklo_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VMRGLH:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_unpacklo_epi16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VMRGLW:
    case PPC_INST_VMRGLW128:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_unpacklo_epi32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VMSUM3FP128:
        // NOTE: accounting for full vector reversal here. should dot product yzw instead of xyz
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_dp_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32), 0xEF));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMSUM4FP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_dp_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32), 0xFF));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VMULFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_mul_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VNMSUBFP:
    case PPC_INST_VNMSUBFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_xor_ps(_mm_sub_ps(_mm_mul_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)), _mm_load_ps({}.f32)), _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000)))));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]), v(insn.operands[3]));
        break;


    case PPC_INST_VNOR:
    case PPC_INST_VNOR128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_xor_si128(_mm_or_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)), _mm_set1_epi32(-1)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VOR:
    case PPC_INST_VOR128:
        print("\t_mm_store_si128((__m128i*){}.u8, ", v(insn.operands[0]));

        if (insn.operands[1] != insn.operands[2])
            println("_mm_or_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[1]), v(insn.operands[2]));
        else
            println("_mm_load_si128((__m128i*){}.u8));", v(insn.operands[1]));

        break;


    case PPC_INST_VPERM:
    case PPC_INST_VPERM128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_perm_epi8_(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]), v(insn.operands[3]));
        break;


    case PPC_INST_VPERMWI128:
    {        // NOTE: accounting for full vector reversal here
        uint32_t x = 3 - (insn.operands[2] & 0x3);
        uint32_t y = 3 - ((insn.operands[2] >> 2) & 0x3);
        uint32_t z = 3 - ((insn.operands[2] >> 4) & 0x3);
        uint32_t w = 3 - ((insn.operands[2] >> 6) & 0x3);
        uint32_t perm = x | (y << 2) | (z << 4) | (w << 6);
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_shuffle_epi32(_mm_load_si128((__m128i*){}.u32), 0x{:X}));", v(insn.operands[0]), v(insn.operands[1]), perm);
        break;
    }

    case PPC_INST_VPKD3D128:
        // TODO: vectorize somehow?
        // NOTE: handling vector reversal here too
        printSetFlushMode(true);
        switch (insn.operands[2])
        {
        case 0: // D3D color
            if (insn.operands[3] != 1)
                fmt::println("Unexpected D3D color pack instruction at {:X}", base);

            for (size_t i = 0; i < 4; i++)
            {
                constexpr size_t indices[] = { 3, 0, 1, 2 };
                println("\t{}.u32[{}] = 0x404000FF;", vTemp(), i);
                println("\t{}.f32[{}] = {}.f32[{}] < 3.0f ? 3.0f : ({}.f32[{}] > {}.f32[{}] ? {}.f32[{}] : {}.f32[{}]);", vTemp(), i, v(insn.operands[1]), i, v(insn.operands[1]), i, vTemp(), i, vTemp(), i, v(insn.operands[1]), i);
                println("\t{}.u32 {}= uint32_t({}.u8[{}]) << {};", temp(), i == 0 ? "" : "|", vTemp(), i * 4, indices[i] * 8);
            }
            println("\t{}.u32[{}] = {}.u32;", v(insn.operands[0]), insn.operands[4], temp());
            break;

        // s13: the two short packs. Derived from the guest code, not from a
        // format table -- NFS MW hits both and trapped on them.
        //
        // These use the same mantissa-bias trick the D3DCOLOR case above
        // decodes: the guest computes 3.0f + k*2^-22 (seen literally as
        // "vspltisw v0,3; vcfsx v0,v0,0; vnmsubfp v0,v1,v13,v0" at 82439FE0),
        // which parks the integer k in the low mantissa bits. D3DCOLOR takes
        // the low BYTE of each float; these take the low 16 BITS.
        //
        // operands[4] is the FIRST destination word, and the pack fills from
        // there through word 3: type 4 has operands[4]==2 and writes 8 bytes
        // (confirmed at 8243A000, whose result is stored as two consecutive
        // words), type 3 has operands[4]==3 and writes 4 bytes (confirmed at
        // 82592118, whose result is read straight back as two halfwords with
        // "lhz r11,80(r1); lhz r10,82(r1)").
        //
        // Source element i lands in destination u16[i+4], exactly as the
        // float16_4 case below does, which puts x,y,z,w in that order in guest
        // memory.
        //
        // s30 FIX -- the clamp FLOOR must be 3.0f MINUS 32768 ulps, not 3.0f.
        // These shorts are SIGNED (SNORM16): the guest biases with a NEGATIVE
        // scale, e.g. sub_82439FE0 does "vnmsubfp v0,v1,v13,v0" with
        // v13 = -32767*2^-22 (the vector at 8208F130), i.e. v0 = 3.0f +
        // x*32767*2^-22.  A negative x therefore lands BELOW 3.0f, where
        // bits(3.0f - m ulps) = 0x40400000 - m, whose low 16 bits are exactly
        // the two's complement of m -- the correct negative short.  Flooring at
        // 3.0f collapsed every negative lane to 0.  That is what broke NFS MW's
        // rear-view mirror: its per-vertex reflection dirs are frustum corners
        // (+-0.755, +-0.170, -1), and the whole negative half plus the constant
        // -1 Z lane came out as 0x0000, flattening the cube lookup onto a face
        // edge (s29 "Z-short=0 at all 4 verts, v2 = null vector").
        // Floor 2.9921875f == 0x403F8000 == integer -32768, the signed
        // saturation point.  The ceiling stays at 65535 ulps so nothing that
        // works today can change: this only widens the accepted range.
        case 3: // 2 shorts
        case 4: // 4 shorts
        {
            const bool four = insn.operands[2] == 4;
            if (four ? (insn.operands[3] != 2 || insn.operands[4] != 2)
                     : (insn.operands[3] != 1 || insn.operands[4] != 3))
                fmt::println("Unexpected short pack instruction at {:X}", base);

            // Clamp every source element into vTemp FIRST. Destination and
            // source are the same register at every site in this game
            // (vpkd3d128 v10,v10,...), so writing the destination while still
            // reading the source would feed packed halves back in as input.
            const size_t first = four ? 0 : 2;
            for (size_t i = first; i < 4; i++)
            {
                println("\t{}.u32[{}] = 0x4040FFFF;", vTemp(), i);
                println("\t{0}.f32[{1}] = {2}.f32[{1}] < 2.9921875f ? 2.9921875f : ({2}.f32[{1}] > {0}.f32[{1}] ? {0}.f32[{1}] : {2}.f32[{1}]);", vTemp(), i, v(insn.operands[1]));
            }
            for (size_t i = first; i < 4; i++)
                println("\t{}.u16[{}] = {}.u16[{}];", v(insn.operands[0]), i + 4, vTemp(), i * 2);
            break;
        }

        // Crackdown (2026-10-14), two fixes:
        //
        // 1. PLACEMENT. operands[3] = pack (2/3 = a 64-bit result), operands[4] = shift,
        //    the WORD the result starts at (the D3DCOLOR case's u32[operands[4]]). The
        //    old code assumed shift 2 everywhere; Crackdown has 20 sites at "5,2,0" and
        //    20 at "5,3,0". Ground truth for shift 0 is 8221F068: the guest parks f31 in
        //    lane 3 (stfs 92(r1); lvx 80(r1)), packs "5,2,0", stores the vector to r1+80
        //    and does "lwz r5,92(r1); sth r5" -- it wants half(w) in the LOW halfword of
        //    the last word, which is exactly the shift-2 layout moved down two words.
        //    For shift < 3, pack 3 places like pack 2 (Xenia's vpkd3d128 permute masks
        //    agree); shift 3 (a single-word w/z placement) is not implemented.
        //
        // 2. DEST == SOURCE. The old loop wrote v(dest).u16[4..7] while still reading
        //    v(src).u32[0..3]; with dest == src, lanes 2 and 3 (y, x) were read AFTER
        //    lane 0/1's halves had overwritten them -> garbage. 102 of Crackdown's 163
        //    sites are "vpkd3d128 vN,vN,5,..." -- and so are 8 of NFS's 10. Halves are
        //    now built in vTemp.u16[4..7] (clear of the u8[0] exponent scratch) and copied
        //    out once every source lane has been read. Same output when dest != src.
        case 5: // float16_4
        {
            const size_t shift = insn.operands[4];
            if ((insn.operands[3] != 2 && insn.operands[3] != 3) || shift > 2)
                fmt::println("Unexpected float16_4 pack instruction at {:X}", base);

            for (size_t i = 0; i < 4; i++)
            {
                // Strip sign from source
                println("\t{}.u32 = ({}.u32[{}]&0x7FFFFFFF);", temp(), v(insn.operands[1]), i);
                // If |source| is > 65504, clamp output to 0x7FFF, else save 8 exponent bits
                println("\t{0}.u8[0] = ({1}.f32 != {1}.f32) || ({1}.f32 > 65504.0f) ? 0xFF : (({2}.u32[{3}]&0x7f800000)>>23);", vTemp(), temp(), v(insn.operands[1]), i);
                // If 8 exponent bits were saved, it can only be 0x8E at most
                // If saved, save first 10 bits of mantissa
                println("\t{}.u16 = {}.u8[0] != 0xFF ? (({}.u32[{}]&0x7FE000)>>13) : 0x0;", temp(), vTemp(), v(insn.operands[1]), i);
                // If saved and > 127-15, exponent is converted from 8 to 5-bit by subtracting 0x70
                // If saved but not > 127-15, clamp exponent at 0, add 0x400 to mantissa and shift right by (0x71-exponent)
                // If right shift is greater than 31 bits, manually clamp mantissa to 0 or else the output of the shift will be wrong
                println("\t{0}.u16[{1}] = {2}.u8[0] != 0xFF ? ({2}.u8[0] > 0x70 ? ((({2}.u8[0]-0x70)<<10)+{3}.u16) : (0x71-{2}.u8[0] > 31 ? 0x0 : ((0x400+{3}.u16)>>(0x71-{2}.u8[0])))) : 0x7FFF;", vTemp(), i + 4, vTemp(), temp());
                // Add back original sign
                println("\t{}.u16[{}] |= (({}.u32[{}]&0x80000000)>>16);", vTemp(), i + 4, v(insn.operands[1]), i);
            }
            for (size_t i = 0; i < 4; i++)
                println("\t{}.u16[{}] = {}.u16[{}];", v(insn.operands[0]), 2 * (shift > 2 ? 2 : shift) + i, vTemp(), i + 4);
            break;
        }

        // Crackdown (2026-10-14): NORMPACKED32, 10:10:10:2 -- the counterpart of
        // VUPKD3D128 type 2 below. DERIVED FROM THE GUEST, like the short packs above:
        // sub_8224C478 builds the input as "vnmsubfp v12,v27,v9,v1" with v1 = 3.0f and
        // v9 = the vector at 0x82085030 = (-511*2^-22 x3, -3*2^-22), i.e.
        //     x,y,z = 3.0f + v*511*2^-22   (SIGNED 10 bits, parked in the low mantissa)
        //     w     = 3.0f + v*3*2^-22     (UNSIGNED 2 bits -- note 3.0f-biased HERE,
        //                                   though the unpack hands w back 1.0f-biased)
        // So: clamp each lane to its field's range in the biased domain (the SNORM floor
        // is 3.0f - 511 ulps, the same reasoning as the s30 short fix), take the low bits,
        // x -> bits 0-9, y -> 10-19, z -> 20-29, w -> 30-31 (D3DDECLTYPE_DEC3N order; the
        // guest code cannot tell x from z, the GPU's vertex fetch does). x is u32[3]
        // (reversed layout, as D3DCOLOR above). Every Crackdown site is "2,1,3" and is
        // followed by "vspltw v12,v12,0" + stvewx, i.e. the word lands in element 0 =
        // u32[operands[4]], the D3DCOLOR convention. A NaN lane takes the floor, as
        // maxps would.
        case 2:
        {
            if (insn.operands[3] != 1)
                fmt::println("Unexpected NORMPACKED32 pack instruction at {:X}", base);

            // our lane, bit position, field mask, biased floor, biased ceiling
            constexpr uint32_t lanes[4][5] = {
                { 3,  0, 0x3FF, 0x403FFE01, 0x404001FF },   // x
                { 2, 10, 0x3FF, 0x403FFE01, 0x404001FF },   // y
                { 1, 20, 0x3FF, 0x403FFE01, 0x404001FF },   // z
                { 0, 30, 0x3,   0x40400000, 0x40400003 },   // w
            };
            println("\t{}.u32 = 0;", temp());
            for (const auto& l : lanes)
            {
                println("\t{}.u32[0] = 0x{:X};", vTemp(), l[3]);
                println("\t{}.u32[1] = 0x{:X};", vTemp(), l[4]);
                println("\t{0}.f32[2] = !({1}.f32[{2}] >= {0}.f32[0]) ? {0}.f32[0] : ({1}.f32[{2}] > {0}.f32[1] ? {0}.f32[1] : {1}.f32[{2}]);", vTemp(), v(insn.operands[1]), l[0]);
                println("\t{}.u32 |= ({}.u32[2] & 0x{:X}) << {};", temp(), vTemp(), l[2], l[1]);
            }
            println("\t{}.u32[{}] = {}.u32;", v(insn.operands[0]), insn.operands[4], temp());
            break;
        }

        default:
            fmt::println("Unimplemented vpkd3d128 type {} at {:X}", insn.operands[2], base);
            println("\t__builtin_trap();");
            break;
        }
        break;

    case PPC_INST_VPKSHSS:
    case PPC_INST_VPKSHSS128:
        println("\t_mm_store_si128((__m128i*){}.s8, _mm_packs_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));",
            v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VPKSHUS:
    case PPC_INST_VPKSHUS128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_packus_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VPKSWSS:
    case PPC_INST_VPKSWSS128:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_packs_epi32(_mm_load_si128((__m128i*){}.s32), _mm_load_si128((__m128i*){}.s32)));",
            v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VPKSWUS:
    case PPC_INST_VPKSWUS128:
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_load_si128((__m128i*){}.s32));", vTemp(), v(insn.operands[2]));
        for (int i = 0; i < 4; i++) {
            println("\t{}.u16[{}] = {}.s32[{}] < 0 ? 0 : ({}.s32[{}] > 0xFFFF ? 0xFFFF : {}.s32[{}]);",
                v(insn.operands[0]), i, vTemp(), i, vTemp(), i, vTemp(), i);
        }
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_load_si128((__m128i*){}.s32));", vTemp(), v(insn.operands[1]));
        for (int i = 0; i < 4; i++) {
            println("\t{}.u16[{}] = {}.s32[{}] < 0 ? 0 : ({}.s32[{}] > 0xFFFF ? 0xFFFF : {}.s32[{}]);",
                v(insn.operands[0]), i + 4, vTemp(), i, vTemp(), i, vTemp(), i);
        }
        break;


    case PPC_INST_VPKUHUM:
        // Pack without saturation - use shuffle to select lower bytes
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_packus_epi16("
            "_mm_and_si128(_mm_load_si128((__m128i*){}.u16), _mm_set1_epi16(0xFF)), "
            "_mm_and_si128(_mm_load_si128((__m128i*){}.u16), _mm_set1_epi16(0xFF))));",
            v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VPKUHUS:
    case PPC_INST_VPKUHUS128:
        // Pack unsigned halfwords to unsigned bytes with saturation
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_packus_epi16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));",
            v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;


    case PPC_INST_VPKUWUM:
    case PPC_INST_VPKUWUM128:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_load_si128((__m128i*){}.u32));", vTemp(), v(insn.operands[2]));
        for (int i = 0; i < 4; i++) {
            println("\t{}.u16[{}] = {}.u16[{}];",
                v(insn.operands[0]), i, vTemp(), i * 2);
        }
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_load_si128((__m128i*){}.u32));", vTemp(), v(insn.operands[1]));
        for (int i = 0; i < 4; i++) {
            println("\t{}.u16[{}] = {}.u16[{}];",
                v(insn.operands[0]), i + 4, vTemp(), i * 2);
        }
        break;


    case PPC_INST_VPKUWUS:
    case PPC_INST_VPKUWUS128:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_load_si128((__m128i*){}.u32));", vTemp(), v(insn.operands[2]));
        for (int i = 0; i < 4; i++) {
            println("\t{}.u16[{}] = {}.u32[{}] > 0xFFFF ? 0xFFFF : {}.u32[{}];",
                v(insn.operands[0]), i, vTemp(), i, vTemp(), i);
        }
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_load_si128((__m128i*){}.u32));", vTemp(), v(insn.operands[1]));
        for (int i = 0; i < 4; i++) {
            println("\t{}.u16[{}] = {}.u32[{}] > 0xFFFF ? 0xFFFF : {}.u32[{}];",
                v(insn.operands[0]), i + 4, vTemp(), i, vTemp(), i);
        }
        break;


    case PPC_INST_VREFP:
    case PPC_INST_VREFP128:
        // TODO: see if we can use rcp safely
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_div_ps(_mm_set1_ps(1), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VRFIM:
    case PPC_INST_VRFIM128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_round_ps(_mm_load_ps({}.f32), _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VRFIN:
    case PPC_INST_VRFIN128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_round_ps(_mm_load_ps({}.f32), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VRFIZ:
    case PPC_INST_VRFIZ128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_round_ps(_mm_load_ps({}.f32), _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VRLIMI128:
    {
        constexpr size_t shuffles[] = { _MM_SHUFFLE(3, 2, 1, 0), _MM_SHUFFLE(2, 1, 0, 3), _MM_SHUFFLE(1, 0, 3, 2), _MM_SHUFFLE(0, 3, 2, 1) };
        println("\t_mm_store_ps({}.f32, _mm_blend_ps(_mm_load_ps({}.f32), _mm_permute_ps(_mm_load_ps({}.f32), {}), {}));", v(insn.operands[0]), v(insn.operands[0]), v(insn.operands[1]), shuffles[insn.operands[3]], insn.operands[2]);
        break;
    }


    case PPC_INST_VRSQRTEFP:
    case PPC_INST_VRSQRTEFP128:
        // TODO: see if we can use rsqrt safely
        // TODO: we can detect if the input is from a dot product and apply logic only on one value
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_div_ps(_mm_set1_ps(1), _mm_sqrt_ps(_mm_load_ps({}.f32))));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VSEL:
    case PPC_INST_VSEL128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_or_si128(_mm_andnot_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)), _mm_and_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8))));", v(insn.operands[0]), v(insn.operands[3]), v(insn.operands[1]), v(insn.operands[3]), v(insn.operands[2]));
        break;


    case PPC_INST_VSL:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_vsl(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VSLB:
        // TODO: vectorize
        for (size_t i = 0; i < 16; i++)
            println("\t{}.u8[{}] = {}.u8[{}] << ({}.u8[{}] & 0x7);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;


    case PPC_INST_VSLH:
        // Vector shift left halfword
        for (size_t i = 0; i < 8; i++)
            println("\t{}.u16[{}] = {}.u16[{}] << ({}.u16[{}] & 0xF);",
                v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;


    case PPC_INST_VSRAH:
        // Vector shift right algebraic halfword
        for (size_t i = 0; i < 8; i++)
            println("\t{}.s16[{}] = {}.s16[{}] >> ({}.u16[{}] & 0xF);",
                v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;


    case PPC_INST_VSRH:
        // Vector shift right halfword
        for (size_t i = 0; i < 8; i++)
            println("\t{}.u16[{}] = {}.u16[{}] >> ({}.u16[{}] & 0xF);",
                v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;


    case PPC_INST_VRLH:
        // Vector rotate left halfword
        for (size_t i = 0; i < 8; i++)
            println("\t{}.u16[{}] = ({}.u16[{}] << ({}.u16[{}] & 0xF)) | "
                "({}.u16[{}] >> (16 - ({}.u16[{}] & 0xF)));",
                v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i,
                v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;


    case PPC_INST_VSLDOI:
    case PPC_INST_VSLDOI128:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_alignr_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8), {}));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]), 16 - insn.operands[3]);
        break;


    case PPC_INST_VSLW:
    case PPC_INST_VSLW128:
        // TODO: vectorize, ensure endianness is correct
        for (size_t i = 0; i < 4; i++)
            println("\t{}.u32[{}] = {}.u32[{}] << ({}.u8[{}] & 0x1F);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i * 4);
        break;


    case PPC_INST_VSPLTB:
    {        // NOTE: accounting for full vector reversal here
        uint32_t perm = 15 - insn.operands[2];
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_shuffle_epi8(_mm_load_si128((__m128i*){}.u8), _mm_set1_epi8(char(0x{:X}))));", v(insn.operands[0]), v(insn.operands[1]), perm);
        break;
    }


    case PPC_INST_VSPLTH:
    {        // NOTE: accounting for full vector reversal here
        uint32_t perm = 7 - insn.operands[2];
        perm = (perm * 2) | ((perm * 2 + 1) << 8);
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_shuffle_epi8(_mm_load_si128((__m128i*){}.u16), _mm_set1_epi16(short(0x{:X}))));", v(insn.operands[0]), v(insn.operands[1]), perm);
        break;
    }


    case PPC_INST_VSPLTISB:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_set1_epi8(char(0x{:X})));", v(insn.operands[0]), insn.operands[1]);
        break;


    case PPC_INST_VSPLTISH:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_set1_epi16(short({})));",
            v(insn.operands[0]), int16_t(insn.operands[1]));
        break;


    case PPC_INST_VSPLTISW:
    case PPC_INST_VSPLTISW128:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_set1_epi32(int(0x{:X})));", v(insn.operands[0]), insn.operands[1]);
        break;


    case PPC_INST_VSPLTW:
    case PPC_INST_VSPLTW128:
    {        // NOTE: accounting for full vector reversal here
        uint32_t perm = 3 - insn.operands[2];
        perm |= (perm << 2) | (perm << 4) | (perm << 6);
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_shuffle_epi32(_mm_load_si128((__m128i*){}.u32), 0x{:X}));", v(insn.operands[0]), v(insn.operands[1]), perm);
        break;
    }


    case PPC_INST_VSR:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_vsr(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VSRAW:
    case PPC_INST_VSRAW128:
        // TODO: vectorize, ensure endianness is correct
        for (size_t i = 0; i < 4; i++)
            println("\t{}.s32[{}] = {}.s32[{}] >> ({}.u8[{}] & 0x1F);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i * 4);
        break;


    case PPC_INST_VSRW:
    case PPC_INST_VSRW128:
        // TODO: vectorize, ensure endianness is correct
        for (size_t i = 0; i < 4; i++)
            println("\t{}.u32[{}] = {}.u32[{}] >> ({}.u8[{}] & 0x1F);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i * 4);
        break;


    case PPC_INST_VSUBFP:
    case PPC_INST_VSUBFP128:
        printSetFlushMode(true);
        println("\t_mm_store_ps({}.f32, _mm_sub_ps(_mm_load_ps({}.f32), _mm_load_ps({}.f32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VSUBSHS:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_subs_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VSUBSWS:
        // TODO: vectorize
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.s64 = int64_t({}.s32[{}]) - int64_t({}.s32[{}]);", temp(), v(insn.operands[1]), i, v(insn.operands[2]), i);
            println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;", v(insn.operands[0]), i, temp(), temp(), temp());
        }
        break;



    case PPC_INST_VSUBUBS:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_subs_epu8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VSUBUHM:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_sub_epi16(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VSUBUWM:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_sub_epi32(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VUPKD3D128:
        // TODO: vectorize somehow?
        // NOTE: handling vector reversal here too
        switch (insn.operands[2] >> 2)
        {
        case 0: // D3D color
            for (size_t i = 0; i < 4; i++)
            {
                constexpr size_t indices[] = { 3, 0, 1, 2 };
                println("\t{}.u32[{}] = {}.u8[{}] | 0x3F800000;", vTemp(), i, v(insn.operands[1]), indices[i]);
            }
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        case 1: // 2 shorts
            for (size_t i = 0; i < 2; i++)
            {
                println("\t{}.f32 = 3.0f;", temp());
                println("\t{}.s32 += {}.s16[{}];", temp(), v(insn.operands[1]), 1 - i);
                println("\t{}.f32[{}] = {}.f32;", vTemp(), 3 - i, temp());
            }
            println("\t{}.f32[1] = 0.0f;", vTemp());
            println("\t{}.f32[0] = 1.0f;", vTemp());
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        // Crackdown (2026-10-14): NORMPACKED32, 10:10:10:2 -- the only unimplemented
        // instruction form in that image (all 24 unpack traps; the other 4 were the
        // matching pack). DERIVED FROM THE GUEST: sub_8224C478 loads the packed word with
        // "lvlx; vsldoi v,v,v,4" (so it sits in the LOW 32 bits, u32[0] -- the same slot
        // the shorts case reads) and removes the bias with "vnmsubfp vD,vU,v13,v0", where
        // v13/v0 come from the vector at 0x82085020 via vpermwi128:
        //     x,y,z = 3*8208.03125 - u*8208.03125 ... = k/511   -> u = 3.0f + k*2^-22,
        //                                                          k SIGNED 10 bits
        //     w     = 2796202.75*(u - 1)            = j/3     -> u = 1.0f + j*2^-23,
        //                                                          j UNSIGNED 2 bits
        // The bias is an integer ADD into the bit pattern (as the shorts case does), so a
        // negative k lands below 3.0f. x is bits 0-9, y 10-19, z 20-29, w 30-31
        // (D3DDECLTYPE_DEC3N order; the guest code cannot tell x from z). The hardware's
        // k == -512 corner is left as a plain value, as the shorts case leaves -32768.
        case 2: // NORMPACKED32
            println("\t{}.u32 = {}.u32[0];", temp(), v(insn.operands[1]));
            for (size_t i = 0; i < 3; i++)
                println("\t{}.u32[{}] = 0x40400000u + uint32_t(int32_t({}.u32 << {}) >> 22);", vTemp(), 3 - i, temp(), 22 - 10 * i);
            println("\t{}.u32[0] = 0x3F800000u | ({}.u32 >> 30);", vTemp(), temp());
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        // s13: half floats. Unlike the "2 shorts" case above, this one yields a
        // DIRECT float: all 8 sites in NFS MW store the result and immediately
        // reload it with lfs, with no 3.0f bias removal anywhere (826B0B98,
        // 8269D388, 826A6070, 82634538 ...). Source halves come from the same
        // slots the shorts case reads -- the guest rotates the data into place
        // with "vsldoi v0,v0,v0,4" first, which is only correct if the unpack
        // reads the low 32 bits of the register.
        //
        // CAUTION: every one of those sites consumes ONLY component x
        // (vspltw v0,v0,0), so x is measured and y/z/w are inferred by symmetry
        // with the shorts case, including its z=0, w=1 tail. Denormal halves
        // flush to a signed zero.
        case 3: // 2 half floats
            for (size_t i = 0; i < 2; i++)
            {
                println("\t{}.u32 = {}.u16[{}];", temp(), v(insn.operands[1]), 1 - i);
                println("\t{0}.u32[{1}] = (({2}.u32 & 0x8000) << 16) | ((({2}.u32 & 0x7C00) == 0) ? 0u : ((({2}.u32 & 0x7C00) == 0x7C00) ? (0x7F800000u | (({2}.u32 & 0x3FF) << 13)) : ((((({2}.u32 >> 10) & 0x1F) + 112u) << 23) | (({2}.u32 & 0x3FF) << 13))));", vTemp(), 3 - i, temp());
            }
            println("\t{}.f32[1] = 0.0f;", vTemp());
            println("\t{}.f32[0] = 1.0f;", vTemp());
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        // Fable II (2026-08-19): float16_4 -- FOUR half floats out of the LOW 64 BITS.
        // This was the ONLY unimplemented instruction in the whole 23 MB image: all 59
        // __builtin_trap() sites in the recompile were this one case, and the game
        // executed one (ud2 in sub_82AAE628 +0x13D3) while streaming music.bnk. xam hits
        // it at 77 more sites, so this is not a Fable-only gap.
        //
        // DERIVED FROM THE GUEST, like the short packs above -- not from a format table:
        //   * FOUR components, EIGHT bytes of source. sub_82234A10 unpacks a vector, then
        //     does "vsldoi v8,v9,v9,8" and unpacks again: rotating by EIGHT to reach the
        //     rest is only correct if one unpack consumes eight bytes. (Case 3, the
        //     2-component one, is rotated by FOUR -- see its comment.)
        //   * HALFWORD granularity: sub_82AAE628 feeds "vsplth v6,v8,0" (splat halfword)
        //     straight into the unpack.
        //   * A DIRECT FLOAT, not the 3.0f-biased integer the short cases produce. Same
        //     proof s13 used for case 3: at 82AAE898 the result is stored with stvewx to
        //     r1+84 and reloaded two instructions later by "lfs f31,84(r1)", with no bias
        //     removal anywhere. (The 18 vsubfp uses in sub_82234A10 are DIFFERENCES of two
        //     unpacked vectors, which cancel a bias and so prove nothing either way --
        //     the stvewx/lfs site is the one that discriminates.)
        //   * Cross-check: VPKD3D128's case 5 is float16_4 as well, derived independently.
        //
        // Element order follows case 3 exactly. That case reads the low 32 bits as
        // u16[1] -> u32[3] (x) and u16[0] -> u32[2] (y); extending the same mapping to the
        // low 64 bits gives u32[k] = half(u16[k]) for k = 3..0, i.e. x,y,z,w from the four
        // halfwords in guest order. No z=0/w=1 tail here -- all four lanes are data.
        //
        // Built in vTemp and assigned at the end because destination and source are the
        // same register at several sites (82234CE4 "vupkd3d128 v8,v26,20" feeds 82234D18
        // "vor v29,v8,v8"); writing the destination early would feed results back in as
        // input. Denormal halves flush to a signed zero, exactly as in case 3.
        case 5: // 4 half floats (float16_4)
            for (size_t i = 0; i < 4; i++)
            {
                println("\t{}.u32 = {}.u16[{}];", temp(), v(insn.operands[1]), 3 - i);
                println("\t{0}.u32[{1}] = (({2}.u32 & 0x8000) << 16) | ((({2}.u32 & 0x7C00) == 0) ? 0u : ((({2}.u32 & 0x7C00) == 0x7C00) ? (0x7F800000u | (({2}.u32 & 0x3FF) << 13)) : ((((({2}.u32 >> 10) & 0x1F) + 112u) << 23) | (({2}.u32 & 0x3FF) << 13))));", vTemp(), 3 - i, temp());
            }
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        // Say WHICH type and WHERE. The bare trap cost a launch to diagnose: it took a
        // map-file relink plus a disassembly to learn that the ud2 was a vupkd3d128 at
        // all. A recompile-time line names it before the game ever runs.
        default:
            fmt::println("Unimplemented vupkd3d128 type {} at {:X}", insn.operands[2] >> 2, base);
            println("\t__builtin_trap();");
            break;
        }
        break;

    case PPC_INST_VUPKHSB:
    case PPC_INST_VUPKHSB128:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_cvtepi8_epi16(_mm_unpackhi_epi64(_mm_load_si128((__m128i*){}.s8), _mm_load_si128((__m128i*){}.s8))));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[1]));
        break;


    case PPC_INST_VUPKHSH:
    case PPC_INST_VUPKHSH128:
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_cvtepi16_epi32(_mm_unpackhi_epi64(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16))));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[1]));
        break;


    case PPC_INST_VUPKLSB:
    case PPC_INST_VUPKLSB128:
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_cvtepi8_epi16(_mm_load_si128((__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VUPKLSH:
    case PPC_INST_VUPKLSH128:
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_cvtepi16_epi32(_mm_load_si128((__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[1]));
        break;


    case PPC_INST_VXOR:
    case PPC_INST_VXOR128:
        print("\t_mm_store_si128((__m128i*){}.u8, ", v(insn.operands[0]));

        if (insn.operands[1] != insn.operands[2])
            println("_mm_xor_si128(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));", v(insn.operands[1]), v(insn.operands[2]));
        else
            println("_mm_setzero_si128());");

        break;


    case PPC_INST_XOR:
        println("\t{}.u64 = {}.u64 ^ {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;


    case PPC_INST_XORI:
        println("\t{}.u64 = {}.u64 ^ {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2]);
        break;


    case PPC_INST_XORIS:
        println("\t{}.u64 = {}.u64 ^ {};", r(insn.operands[0]), r(insn.operands[1]), insn.operands[2] << 16);
        break;


    case PPC_INST_BDZF:
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\t--{}.u64;", ctr());
        if (insn.operands[1] < fn.base || insn.operands[1] >= fn.base + fn.size)
        {
            println("\tif ({}.u32 == 0 && !{}.{}) {{", ctr(), cr(insn.operands[0] / 4), fields[insn.operands[0] % 4]);
            print("\t");
            printFunctionCall(insn.operands[1]);
            println("\t\tPPC_RETURN();");
            println("\t}}");
        }
        else
        {
            println("\tif ({}.u32 == 0 && !{}.{}) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), fields[insn.operands[0] % 4], insn.operands[1]);
        }
        break;
    }


    case PPC_INST_CROR:
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\t{}.{} = {}.{} | {}.{};", cr(insn.operands[0] / 4), fields[insn.operands[0] % 4], cr(insn.operands[1] / 4), fields[insn.operands[1] % 4], cr(insn.operands[2] / 4), fields[insn.operands[2] % 4]);
        break;
    }


    case PPC_INST_CRORC:
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\t{}.{} = {}.{} | (~{}.{} & 1);", cr(insn.operands[0] / 4), fields[insn.operands[0] % 4], cr(insn.operands[1] / 4), fields[insn.operands[1] % 4], cr(insn.operands[2] / 4), fields[insn.operands[2] % 4]);
        break;
    }


   

    case PPC_INST_FNMADD:
        printSetFlushMode(false);
        println("\t{}.f64 = -std::fma({}.f64, {}.f64, {}.f64);", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        // No CR1 update on the Rc form: PPCFPSCRRegister has no setFlags(), so
        // this emitted code never compiled -- it was simply unreachable until
        // the memory-image loader fix moved .text and exposed the four Rc-form
        // FP instructions in this kernel. Every sibling (FMADD, FMSUB,
        // FNMADDS) also ignores Rc and relies on the generic
        // "RC bit enabled but no comparison was generated" warning; match them.
        break;


    case PPC_INST_LBZUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U8({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_LDUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U64({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_LFDU:
        printSetFlushMode(false);
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U64({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LFDUX:
        printSetFlushMode(false);
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U64({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_LFSU:
        printSetFlushMode(false);
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
        break;


    case PPC_INST_LFSUX:
        printSetFlushMode(false);
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
        break;


    case PPC_INST_LHAU:
        print("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        print("\t{}.s64 = int16_t(PPC_LOAD_U16({}));", r(insn.operands[0]), ea());
        print("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LHZU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U16({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LHZUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U16({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    // s31: needed by dash.xex (3 sites, 0x926586FC/0x92658700/0x92658F1C). Same shape
    // as LHZUX but sign-extended, exactly as LHAU is to LHZU.
    case PPC_INST_LHAUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.s64 = int16_t(PPC_LOAD_U16({}));", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_LVEBX:
    case PPC_INST_LVEWX:
    case PPC_INST_LVEWX128:
    case PPC_INST_LVXL:
    case PPC_INST_LVX:
    case PPC_INST_LVX128:
    case PPC_INST_LVXL128:
    case PPC_INST_LVEHX: {
        print("\t_mm_store_si128((__m128i*){}.u8, _mm_shuffle_epi8(_mm_load_si128((__m128i*)(base + ((", v(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32) & ~0xF))), _mm_load_si128((__m128i*)VectorMaskL)));", r(insn.operands[2]));
        break;


    case PPC_INST_LWZUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.u64 = PPC_LOAD_U32({});", r(insn.operands[0]), ea());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_STBUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u8);", mmioStore() ? "PPC_MM_STORE_U8(" : "PPC_STORE_U8(", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_STDUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u64);", mmioStore() ? "PPC_MM_STORE_U64(" : "PPC_STORE_U64(", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_STFDU:
        printSetFlushMode(false);
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u64);", mmioStore() ? "PPC_MM_STORE_U64(" : "PPC_STORE_U64(", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_STFSU:
        printSetFlushMode(false);
        println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u32);", mmioStore() ? "PPC_MM_STORE_U32(" : "PPC_STORE_U32(", ea(), temp());
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_LHBRX:
        print("\t{}.u64 = __builtin_bswap16(PPC_LOAD_U16(", r(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32));", r(insn.operands[2]));
        break;


    case PPC_INST_STFSUX:
        printSetFlushMode(false);
        println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u32);", mmioStore() ? "PPC_MM_STORE_U32(" : "PPC_STORE_U32(", ea(), temp());
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;


    case PPC_INST_STHU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u16);", mmioStore() ? "PPC_MM_STORE_U16(" : "PPC_STORE_U16(", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;


    case PPC_INST_STHUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}{}, {}.u16);", mmioStore() ? "PPC_MM_STORE_U16(" : "PPC_STORE_U16(", ea(), r(insn.operands[0]));
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;

    case PPC_INST_STVEBX:
        // Compute 16-byte aligned effective address (as required by STVEX family)
        println("\t{} = ({}.u32 + {}.u32) & ~0xF;", ea(), r(insn.operands[1]), r(insn.operands[2]));

        // Compute the index within the vector (big-endian style)
        println("\tPPC_STORE_U8({}, {}.u8[15 - (({}.u32 + {}.u32) & 0xF)]);",
            ea(),
            v(insn.operands[0]),
            r(insn.operands[1]), r(insn.operands[2]));
        break;


    case PPC_INST_MTCRF:
        // Like MTCR, but only the CR fields selected by the CRM mask (operand 0).
        // CRM bit 0x80 selects CR0 (the most significant field).
        for (size_t i = 0; i < 32; i++)
        {
            if (insn.operands[0] & (0x80u >> (i / 4)))
            {
                constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
                println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;", cr(i / 4), fields[i % 4], r(insn.operands[1]), 1u << (31 - i));
            }
        }
        break;


    case PPC_INST_BGTLA:
        printSetFlushMode(true);
        // CR[BI]'s GT bit is at bit 1 in the 4-bit field: mask is 0b0100 = 0x4
        println("\tif ((state.cr & (0x4 << (4 * (7 - {})))) != 0) {{", insn.operands[0]);
        println("\t\tADJ(ctx)->lr = 0x{:X};  // Link to next instruction", base + 4);
        println("\t\treturn 0x{:X};    // Branch absolute", uint32_t(insn.operands[1]));
        println("\t}}");
        break;


    case PPC_INST_VADDSBS:
        println("\t_mm_store_si128((__m128i*){}.s8, _mm_adds_epi8(_mm_load_si128((__m128i*){}.s8), _mm_load_si128((__m128i*){}.s8)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDSWS:
        println("\t_mm_store_si128((__m128i*){}.u32, _mm_adds_epi32(_mm_load_si128((__m128i*){}.u32), _mm_load_si128((__m128i*){}.u32)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VADDUHS:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_adds_epu16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
    break;
    
    case PPC_INST_VCMPEQUH:
        println("\t_mm_store_si128((__m128i*){}.u16, _mm_cmpeq_epi16(_mm_load_si128((__m128i*){}.u16), _mm_load_si128((__m128i*){}.u16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_si128((__m128i*){}.u16), 0xFFFF);", cr(6), v(insn.operands[0]));
        break;

    case PPC_INST_VCMPGTSB:
        println("\t_mm_store_si128((__m128i*){}.s8, _mm_cmpgt_epi8(_mm_load_si128((__m128i*){}.s8), _mm_load_si128((__m128i*){}.s8)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_si128((__m128i*){}.s8), 0xFFFF);", cr(6), v(insn.operands[0]));
        break;

    case PPC_INST_VCMPGTSH:
        println("\t_mm_store_si128((__m128i*){}.s16, _mm_cmpgt_epi16(_mm_load_si128((__m128i*){}.s16), _mm_load_si128((__m128i*){}.s16)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_si128((__m128i*){}.s16), 0xFFFF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VCMPGTSW:
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_cmpgt_epi32(_mm_load_si128((__m128i*){}.s32), _mm_load_si128((__m128i*){}.s32)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(_mm_load_si128((__m128i*){}.s32), 0xFFFF);", cr(6), v(insn.operands[0]));
        break;


    case PPC_INST_VMINSW:
        printSetFlushMode(true);
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_min_epi32(_mm_load_si128((__m128i*){}.s32), _mm_load_si128((__m128i*){}.s32)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_VAVGSW:
        printSetFlushMode(true);
        println("\t{{");
        println("\t__m128i sum = _mm_add_epi32(_mm_load_si128((__m128i*){}.s32), _mm_load_si128((__m128i*){}.s32));", v(insn.operands[1]), v(insn.operands[2]));
        println("\tsum = _mm_add_epi32(sum, _mm_set1_epi32(1));");
        println("\t_mm_store_si128((__m128i*){}.s32, _mm_srai_epi32(sum, 1));", v(insn.operands[0]));
        println("\t}}");
        break;


    case PPC_INST_VSLO:
    case PPC_INST_VSLO128:
        printSetFlushMode(true);
        println("\t{{");
        println("\tint shift = ({}.u8[15] >> 3) & 0xF;", v(insn.operands[2]));
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_setzero_si128());", v(insn.operands[0]));
        println("\tmemcpy({}.u8, {}.u8 + shift, 16 - shift);", v(insn.operands[0]), v(insn.operands[1]));
        println("\t}}");
        break;

    case PPC_INST_VSRO:
    case PPC_INST_VSRO128:
        printSetFlushMode(true);
        println("\t{{");
        println("\tint shift = ({}.u8[15] >> 3) & 0xF;", v(insn.operands[2]));
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_setzero_si128());", v(insn.operands[0]));
        println("\tmemcpy({}.u8 + shift, {}.u8, 16 - shift);", v(insn.operands[0]), v(insn.operands[1]));
        println("\t}}");
        break;

    case PPC_INST_VSUBSBS:
        printSetFlushMode(true);
        println("\t_mm_store_si128((__m128i*){}.s8, _mm_subs_epi8(_mm_load_si128((__m128i*){}.s8), _mm_load_si128((__m128i*){}.s8)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VSRAB:
        // TODO: vectorize
        for (size_t i = 0; i < 16; i++)
            println("\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;


    case PPC_INST_VSUBUBM:
        println("\t_mm_store_si128((__m128i*){}.u8, _mm_sub_epi8(_mm_load_si128((__m128i*){}.u8), _mm_load_si128((__m128i*){}.u8)));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;


    case PPC_INST_BSO:
        printConditionalBranch(false, "so");
        break;


    case PPC_INST_BSOLR:
        println("\tif ({}.so) PPC_RETURN();", cr(insn.operands[0]));
        break;


    case PPC_INST_BNS:
        printConditionalBranch(true, "so");
        break;


    case PPC_INST_BNSLR:
        println("\tif (!{}.so) PPC_RETURN();", cr(insn.operands[0]));
        break;

    }

    default:
        return false;
    }

#if 1
                         if (strchr(insn.opcode->name, '.'))
                         {
                             int lastLine = out.find_last_of('\n', out.size() - 2);
                             if (out.find("cr0", lastLine + 1) == std::string::npos && out.find("cr6", lastLine + 1) == std::string::npos)
                                 fmt::println("{} at {:X} has RC bit enabled but no comparison was generated", insn.opcode->name, base);
                         }
#endif

                         if (midAsmHook != config.midAsmHooks.end() && midAsmHook->second.afterInstruction)
                             printMidAsmHook();

                         return true;
}

bool Recompiler::Recompile(const Function& fn)
{
    auto base = fn.base;
    auto end = base + fn.size;
    auto* data = (uint32_t*)image.Find(base);

    static std::unordered_set<size_t> labels;
    labels.clear();

    for (size_t addr = base; addr < end; addr += 4)
    {
        const uint32_t instruction = ByteSwap(*(uint32_t*)((char*)data + addr - base));
        if (!PPC_BL(instruction))
        {
            const size_t op = PPC_OP(instruction);
            if (op == PPC_OP_B)
                labels.emplace(addr + PPC_BI(instruction));
            else if (op == PPC_OP_BC)
                labels.emplace(addr + PPC_BD(instruction));
        }

        auto switchTable = config.switchTables.find(addr);
        if (switchTable != config.switchTables.end())
        {
            for (auto label : switchTable->second.labels)
                labels.emplace(label);
        }

        auto midAsmHook = config.midAsmHooks.find(addr);
        if (midAsmHook != config.midAsmHooks.end())
        {
            if (midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                midAsmHook->second.jumpAddressOnFalse != NULL || midAsmHook->second.jumpAddressOnTrue != NULL)
            {
                print("extern bool ");
            }
            else
            {
                print("extern void ");
            }

            print("{}(", midAsmHook->second.name);
            for (auto& reg : midAsmHook->second.registers)
            {
                if (out.back() != '(')
                    out += ", ";

                switch (reg[0])
                {
                case 'c':
                    if (reg == "ctr")
                        print("PPCRegister& ctr");
                    else
                        print("PPCCRRegister& {}", reg);
                    break;

                case 'x':
                    print("PPCXERRegister& xer");
                    break;

                case 'r':
                    print("PPCRegister& {}", reg);
                    break;

                case 'f':
                    if (reg == "fpscr")
                        print("PPCFPSCRRegister& fpscr");
                    else
                        print("PPCRegister& {}", reg);
                    break;

                case 'v':
                    print("PPCVRegister& {}", reg);
                    break;
                }
            }

            println(");\n");

            if (midAsmHook->second.jumpAddress != NULL)
                labels.emplace(midAsmHook->second.jumpAddress);
            if (midAsmHook->second.jumpAddressOnTrue != NULL)
                labels.emplace(midAsmHook->second.jumpAddressOnTrue);
            if (midAsmHook->second.jumpAddressOnFalse != NULL)
                labels.emplace(midAsmHook->second.jumpAddressOnFalse);
        }
    }

    auto symbol = image.symbols.find(fn.base);
    std::string name;
    if (symbol != image.symbols.end())
    {
        name = symbol->name;
    }
    else
    {
        name = fmt::format("sub_{}", fn.base);
    }

#ifdef XENON_RECOMP_USE_ALIAS
    println("__attribute__((alias(\"__imp__{}\"))) PPC_WEAK_FUNC({});", name, name);
#endif

    println("PPC_FUNC_IMPL(__imp__{}) {{", name);
    println("\tPPC_FUNC_PROLOGUE();");

    auto switchTable = config.switchTables.end();
    bool allRecompiled = true;
    CSRState csrState = CSRState::Unknown;

    // TODO: the printing scheme here is scuffed
    RecompilerLocalVariables localVariables;
    static std::string tempString;
    tempString.clear();
    std::swap(out, tempString);

    ppc_insn insn;
    while (base < end)
    {
        if (labels.find(base) != labels.end())
        {
            println("loc_{:X}:", base);

            // Anyone could jump to this label so we wouldn't know what the CSR state would be.
            csrState = CSRState::Unknown;
        }

        if (switchTable == config.switchTables.end())
            switchTable = config.switchTables.find(base);

        ppc::Disassemble(data, 4, base, insn);


        if (insn.opcode == nullptr)
        {
            println("\t// {}", insn.op_str);
            // Big-endian 0x07FF70NN (raw LE read 0xNN70FF07) is an XeO3
            // host-import stub, not a PPC instruction: ordinal 0x70NN in DX,
            // dispatched through PrecompiledPointers[1]. See PPC_IMPORT in
            // ppc_context.h. The kernel's import table at 0x800BF240 is made
            // entirely of these.
            if ((*data & 0x00FFFFFF) == 0x0070FF07)
            {
                const uint32_t ordinal = 0x7000 | (*data >> 24);
                // The ce kernel's ordinals use its donor Emu's numbering; the
                // target Emu renumbered the list. Dispatch only VERIFIED
                // translations; everything else becomes a logged no-op via
                // PPC_IMPORT_UNMAPPED so the boot reveals the worklist.
                // Verified so far (semantics checked in both Emu binaries;
                // 0x704B/0x704E/0x704F additionally by positional alignment
                // of homologous ce/cf call sites -- follow_helper.py):
                //   0x7049 (commit guest pages)        -> 0x7036
                //   0x704B (query pages, r6 = out ptr) -> 0x7038
                //   0x704E (spawn host worker thread)  -> 0x703E (Emu: ret 0)
                //   0x704F (two-guest-pointer op)      -> 0x703B
                static const std::unordered_map<uint32_t, uint32_t> ceToEmuOrdinal =
                {
                    { 0x7049, 0x7036 },
                    { 0x704B, 0x7038 },
                    { 0x704E, 0x703E },
                    { 0x704F, 0x703B },
                };
                // Ordinals with NO counterpart in the target Emu's dialect,
                // implemented locally in the DLL (see PPC_IMPORT_LOCAL in
                // ppc_context.h). The fusion kernel's homologs of these
                // sites replace the import with native code: 0x7013 is the
                // IRQL-change notify (cf omits it), 0x70A3/0x70A4 the global
                // spinlock acquire/release (cf: lwarx/stwcx. on its own
                // guest word; the donor Emu: one host critical section).
                // s9: the idle-loop services. ce's per-processor worker pump
                // (sub_80063300) spins 0x7052 -> 0x709F -> 0x7014 ~50M/s when
                // they are silent no-ops. Donor Emu semantics (disassembled):
                //   0x7052 WaitForSingleObject(perProcWorkEvent, proc0?1ms:INF)
                //   0x7014 dequeue up to r5 host work items into r4, r3=count
                //   0x709F "work pending?" poll read of per-proc host state
                //   0x709E/0x7012 lock/barrier around the DPC retire
                // cf's homolog of the DPC function (ce 0x80062478 = cf
                // 0x80062A48, byte-identical body) OMITS 709F/709E/7012
                // entirely, and cf has no homolog of the worker pump at all --
                // this Emu's dialect has none of these services. Local impl:
                // 0x7052 real 1ms event wait, 0x7014/0x709F defined r3=0,
                // 0x709E/0x7012 no-ops.
                static const std::unordered_set<uint32_t> localOrdinals =
                {
                    0x7013, 0x70A3, 0x70A4,
                    0x7012, 0x7014, 0x7052, 0x709E, 0x709F,
                };
                auto remap = ceToEmuOrdinal.find(ordinal);
                if (remap != ceToEmuOrdinal.end())
                    println("\tPPC_IMPORT(0x{:X}, 0x{:X}); // ce 0x{:X} -> emu dialect", base, remap->second, ordinal);
                else if (localOrdinals.count(ordinal))
                    println("\tPPC_IMPORT_LOCAL(0x{:X}, 0x{:X});", base, ordinal);
                else
                    println("\tPPC_IMPORT_UNMAPPED(0x{:X}, 0x{:X});", base, ordinal);
            }
#if 1
            else if (*data != 0)
                fmt::println("Unable to decode instruction {:X} at {:X}", *data, base);
#endif
        }
        else
        {
            if (insn.opcode->id == PPC_INST_BCTR && (*(data - 1) == 0x07008038 || *(data - 1) == 0x00000060) && switchTable == config.switchTables.end())
                fmt::println("Found a switch jump table at {:X} with no switch table entry present", base);

            if (!Recompile(fn, base, insn, data, switchTable, localVariables, csrState))
            {
                fmt::println("Unrecognized instruction at 0x{:X}: {}", base, insn.opcode->name);
                allRecompiled = false;
            }
        }

        base += 4;
        ++data;
    }

#if 0
    if (insn.opcode == nullptr || (insn.opcode->id != PPC_INST_B && insn.opcode->id != PPC_INST_BCTR && insn.opcode->id != PPC_INST_BLR))
        fmt::println("Function at {:X} ends prematurely with instruction {} at {:X}", fn.base, insn.opcode != nullptr ? insn.opcode->name : "INVALID", base - 4);
#endif

    // A function whose last instruction is not an unconditional terminator does
    // not end -- execution simply continues at the next guest address. That is
    // the normal case for every function produced by SPLITTING another one (the
    // 1836 boundaries fix_calls.py adds, and every entry of a per-instruction
    // build), so returning here would silently cut the guest off mid-routine.
    // Fall through into whatever lives at fn.base + fn.size instead.
    // PPC_RETURN() stays as the fallback when there is nothing to fall into.
    {
        bool terminated = insn.opcode != nullptr &&
            (insn.opcode->id == PPC_INST_B ||
             insn.opcode->id == PPC_INST_BLR ||
             insn.opcode->id == PPC_INST_BCTR);
        uint32_t next = fn.base + fn.size;
        auto nextSymbol = image.symbols.find(next);
        bool canFallThrough = !terminated &&
            nextSymbol != image.symbols.end() &&
            nextSymbol->address == next &&
            (nextSymbol->type == Symbol_Function || nextSymbol->type == Symbol_Import);

        if (canFallThrough)
        {
            println("\t// fall through to {:X}", next);
            println("\t{}(ctx, base, frame);", nextSymbol->name);
        }
        println("\tPPC_RETURN();");
    }
    println("}}\n");

#ifndef XENON_RECOMP_USE_ALIAS
    println("PPC_WEAK_FUNC({}) {{", name);
    println("\treturn __imp__{}(ctx, base, frame);", name);
    println("}}\n");
#endif

    std::swap(out, tempString);
    if (localVariables.ctr)
        println("\tPPCRegister ctr{{}};");
    if (localVariables.xer)
        println("\tPPCXERRegister xer{{}};");
    if (localVariables.reserved)
        println("\tPPCRegister reserved{{}};");

    for (size_t i = 0; i < 8; i++)
    {
        if (localVariables.cr[i])
            println("\tPPCCRRegister cr{}{{}};", i);
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (localVariables.r[i])
            println("\tPPCRegister r{}{{}};", i);
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (localVariables.f[i])
            println("\tPPCRegister f{}{{}};", i);
    }

    for (size_t i = 0; i < 128; i++)
    {
        if (localVariables.v[i])
            println("\tPPCVRegister v{}{{}};", i);
    }

    if (localVariables.env)
        println("\tPPCContext env{{}};");

    if (localVariables.temp)
        println("\tPPCRegister temp{{}};");

    if (localVariables.vTemp)
        println("\tPPCVRegister vTemp{{}};");

    if (localVariables.ea)
        println("\tuint32_t ea{{}};");

    out += tempString;

    return allRecompiled;
}

void Recompiler::Recompile(const std::filesystem::path& headerFilePath)
{
    out.reserve(10 * 1024 * 1024);

    {
        println("#pragma once");

        println("#ifndef PPC_CONFIG_H_INCLUDED");
        println("#define PPC_CONFIG_H_INCLUDED\n");

        if (config.skipLr)
            println("#define PPC_CONFIG_SKIP_LR");
        if (config.ctrAsLocalVariable)
            println("#define PPC_CONFIG_CTR_AS_LOCAL");
        if (config.xerAsLocalVariable)
            println("#define PPC_CONFIG_XER_AS_LOCAL");
        if (config.reservedRegisterAsLocalVariable)
            println("#define PPC_CONFIG_RESERVED_AS_LOCAL");
        if (config.skipMsr)
            println("#define PPC_CONFIG_SKIP_MSR");
        if (config.crRegistersAsLocalVariables)
            println("#define PPC_CONFIG_CR_AS_LOCAL");
        if (config.nonArgumentRegistersAsLocalVariables)
            println("#define PPC_CONFIG_NON_ARGUMENT_AS_LOCAL");
        if (config.nonVolatileRegistersAsLocalVariables)
            println("#define PPC_CONFIG_NON_VOLATILE_AS_LOCAL");

        println("");

        println("#define PPC_IMAGE_BASE 0x{:X}ull", image.base);
        println("#define PPC_IMAGE_SIZE 0x{:X}ull", image.size);

        // Extract the address of the minimum code segment to store the function table at.
        size_t codeMin = ~0;
        size_t codeMax = 0;

        for (auto& section : image.sections)
        {
            if ((section.flags & SectionFlags_Code) != 0)
            {
                if (section.base < codeMin)
                    codeMin = section.base;

                if ((section.base + section.size) > codeMax)
                    codeMax = (section.base + section.size);
            }
        }

        println("#define PPC_CODE_BASE 0x{:X}ull", codeMin);
        println("#define PPC_CODE_SIZE 0x{:X}ull", codeMax - codeMin);

        println("");

        println("#ifdef PPC_INCLUDE_DETAIL");
        println("#include \"ppc_detail.h\"");
        println("#endif");

        println("\n#endif");

        SaveCurrentOutData("ppc_config.h");
    }

    {
        println("#pragma once");

        println("#include \"ppc_config.h\"\n");

        std::ifstream stream(headerFilePath);
        if (stream.good())
        {
            std::stringstream ss;
            ss << stream.rdbuf();
            out += ss.str();
        }

        SaveCurrentOutData("ppc_context.h");
    }

    {
        println("#pragma once\n");
        println("#include \"ppc_config.h\"");
        println("#include \"ppc_context.h\"\n");

        for (auto& symbol : image.symbols)
            println("PPC_EXTERN_FUNC({});", symbol.name);

        SaveCurrentOutData("ppc_recomp_shared.h");
    }

    {
        println("#include \"ppc_recomp_shared.h\"\n");

        println("PPCFuncMapping PPCFuncMappings[] = {{");
        for (auto& symbol : image.symbols)
            println("\t{{ 0x{:X}, 0 }},", symbol.address - image.base, symbol.name);
        println("}};");

        println("");

        println("PPCFunc* HostFuncs[] = {{");
        for (auto& symbol : image.symbols)
            println("\t&{},", symbol.name);
        println("}};");
        println("int HostFuncsCount = sizeof(HostFuncs) / sizeof(PPCFunc*);");

        SaveCurrentOutData("ppc_func_mapping.cpp");
    }

    {
        println("#include \"ppc_recomp_shared.h\"\n");

        println("__declspec(dllexport) PPCImportMapping PrecompiledImportTable[] = {{");
        for (auto& symbol : image.symbols) {
            if (symbol.type == Symbol_Import) println("\t{{ 0x{:X}, 0 }},", symbol.address);
        }
        println("\t{{ 0, 0 }},");
        println("\t{{ 0xFFFFFFFF, 0 }},");
        println("}};");

        println("");

        int i = 0;
        for (auto& symbol : image.symbols) {
            if (symbol.type == Symbol_Import) {
                println("PPC_IMPORT_FUNC_IMPL({}, {})", symbol.name, i);
                i++;
            }
        }

        SaveCurrentOutData("ppc_import_mapping.cpp");
    }

    for (size_t i = 0; i < functions.size(); i++)
    {
        if ((i % 256) == 0)
        {
            SaveCurrentOutData();
            println("#include \"ppc_recomp_shared.h\"\n");
        }

        if ((i % 2048) == 0 || (i == (functions.size() - 1)))
            fmt::println("Recompiling functions... {}%", static_cast<float>(i + 1) / functions.size() * 100.0f);

        Recompile(functions[i]);
    }

    SaveCurrentOutData();
}

void Recompiler::SaveCurrentOutData(const std::string_view& name)
{
    if (!out.empty())
    {
        std::string cppName;

        if (name.empty())
        {
            cppName = fmt::format("ppc_recomp.{}.cpp", cppFileIndex);
            ++cppFileIndex;
        }

        bool shouldWrite = true;

        // Check if an identical file already exists first to not trigger recompilation
        std::string directoryPath = config.directoryPath;
        if (!directoryPath.empty())
            directoryPath += "/";

        std::string filePath = fmt::format("{}{}/{}", directoryPath, config.outDirectoryPath, name.empty() ? cppName : name);
        FILE* f = fopen(filePath.c_str(), "rb");
        if (f)
        {
            static std::vector<uint8_t> temp;

            fseek(f, 0, SEEK_END);
            long fileSize = ftell(f);
            if (fileSize == out.size())
            {
                fseek(f, 0, SEEK_SET);
                temp.resize(fileSize);
                fread(temp.data(), 1, fileSize, f);

                shouldWrite = !XXH128_isEqual(XXH3_128bits(temp.data(), temp.size()), XXH3_128bits(out.data(), out.size()));
            }
            fclose(f);
        }

        if (shouldWrite)
        {
            f = fopen(filePath.c_str(), "wb");
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }

        out.clear();
    }
}

