#include "Emitter.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace UdonLuau::Detail {

    namespace {

        std::string ValueKey(const HeapValue& v) {
            std::string key = std::format("{}|{}|{}|{}|{}|{}", static_cast<int>(v.kind), v.boolean, v.integer, v.unsignedInteger, v.real, v.text);
            for (const HeapValue& a : v.arguments) key += "(" + ValueKey(a) + ")";
            return key;
        }

        std::string ConstantKey(const Type* type, const HeapValue& v) {
            return type->udonName + "|" + ValueKey(v);
        }

    } // namespace

    std::string Emitter::Unique(std::string base) {
        int& n = nameCounters_[base];
        return std::format("{}_{}", base, n++);
    }

    uint32_t Emitter::AddSlot(std::string symbol, const Type* type, HeapValue value, bool exported) {
        auto address = static_cast<uint32_t>(heap_.size());
        heap_.push_back({ std::move(symbol), type->udonName, std::move(value), exported, {} });
        slotTypes_.push_back(type);
        isTemp_.push_back(false);
        isConstant_.push_back(false);
        return address;
    }

    uint32_t Emitter::Constant(const Type* type, const HeapValue& value) {
        std::string key = ConstantKey(type, value);
        if (auto it = constants_.find(key); it != constants_.end()) return it->second;
        uint32_t slot = AddSlot(Unique("__const_" + type->udonName), type, value);
        isConstant_[slot] = true;
        constants_.emplace(std::move(key), slot);
        return slot;
    }

    uint32_t Emitter::JumpTable(const Type* arrayType, const Type* elementType, const std::vector<Label>& targets) {
        HeapValue table;
        table.kind = ValueKind::Array;
        table.text = elementType->udonName;
        HeapValue entry;
        entry.kind = ValueKind::Unsigned;
        table.arguments.assign(targets.size(), entry);
        uint32_t slot = AddSlot(Unique("__gintnl_" + arrayType->udonName), arrayType, std::move(table));
        isConstant_[slot] = true;
        for (size_t i = 0; i < targets.size(); ++i) elementFixups_.emplace_back(slot, i, targets[i].id);
        return slot;
    }

    void Emitter::MarkConstant(uint32_t slot) {
        isConstant_[slot] = true;
    }

    bool Emitter::IsConstant(uint32_t slot) const {
        return slot < isConstant_.size() && isConstant_[slot];
    }

    void Emitter::ResetTemps() {
        freeTemps_.clear();
        liveTemps_.clear();
        reads_.clear();
    }

    std::optional<uint32_t> Emitter::CachedRead(uint32_t externSlot, uint32_t receiver) const {
        if (auto it = reads_.find({ externSlot, receiver }); it != reads_.end()) return it->second;
        return std::nullopt;
    }

    void Emitter::RememberRead(uint32_t externSlot, uint32_t receiver, uint32_t result) {
        if (receiver != result) reads_[{ externSlot, receiver }] = result;
    }

    void Emitter::Forget(uint32_t slot) {
        std::erase_if(reads_, [slot](const auto& entry) { return entry.first.second == slot || entry.second == slot; });
    }

    void Emitter::Promote(uint32_t slot) {
        if (auto it = std::find(liveTemps_.rbegin(), liveTemps_.rend(), slot); it != liveTemps_.rend()) liveTemps_.erase(std::next(it).base());
        isTemp_[slot] = false;
    }

    uint32_t Emitter::ExternSlot(const std::string& signature) {
        if (auto it = externs_.find(signature); it != externs_.end()) return it->second;
        HeapValue v;
        v.kind = ValueKind::String;
        v.text = signature;
        heap_.push_back({ Unique("__extern"), "SystemString", std::move(v), false, {} });
        slotTypes_.push_back(nullptr);
        isTemp_.push_back(false);
        isConstant_.push_back(true);
        auto slot = static_cast<uint32_t>(heap_.size() - 1);
        externs_.emplace(signature, slot);
        return slot;
    }

    uint32_t Emitter::AddressConstant(Label target) {
        HeapValue v;
        v.kind = ValueKind::Unsigned;
        heap_.push_back({ Unique("__gintnl_SystemUInt32"), "SystemUInt32", v, false, {} });
        slotTypes_.push_back(nullptr);
        isTemp_.push_back(false);
        isConstant_.push_back(true);
        auto slot = static_cast<uint32_t>(heap_.size() - 1);
        slotFixups_.emplace_back(slot, target.id);
        return slot;
    }

    uint32_t Emitter::Local(std::string_view name, const Type* type) {
        return AddSlot(Unique(std::format("__lcl_{}_{}", name, type->udonName)), type);
    }

    uint32_t Emitter::Hidden(const Type* type) {
        return AddSlot(Unique("__intnl_" + type->udonName), type);
    }

    uint32_t Emitter::Temp(const Type* type) {
        auto& pool = freeTemps_[type];
        uint32_t slot;
        if (!pool.empty()) {
            slot = pool.back();
            pool.pop_back();
        } else {
            slot = AddSlot(Unique("__intnl_" + type->udonName), type);
            isTemp_[slot] = true;
        }
        liveTemps_.push_back(slot);
        return slot;
    }

    void Emitter::ReleaseTemps(size_t mark) {
        while (liveTemps_.size() > mark) {
            uint32_t slot = liveTemps_.back();
            liveTemps_.pop_back();
            freeTemps_[slotTypes_[slot]].push_back(slot);
            Forget(slot);
        }
    }

    bool Emitter::IsTemp(uint32_t slot) const {
        return slot < isTemp_.size() && isTemp_[slot];
    }

    void Emitter::MarkLine(int line) {
        uint32_t here = Here();
        if (!lines_.empty() && lines_.back().address == here) {
            lines_.back().line = line;
            return;
        }
        if (!lines_.empty() && lines_.back().line == line) return;
        lines_.push_back({ here, line });
    }

    Label Emitter::NewLabel() {
        labels_.emplace_back();
        return { static_cast<int>(labels_.size() - 1) };
    }

    void Emitter::Bind(Label label) {
        labels_[label.id] = Here();
        lastResultOperand_.reset();
        reads_.clear();
    }

    std::optional<uint32_t> Emitter::AddressOf(Label label) const {
        return labels_[label.id];
    }

    void Emitter::Emit(OpCode op) {
        code_.push_back(static_cast<uint32_t>(op));
        lastResultOperand_.reset();
    }

    void Emitter::Emit(OpCode op, uint32_t operand) {
        code_.push_back(static_cast<uint32_t>(op));
        code_.push_back(operand);
        lastResultOperand_.reset();
    }

    void Emitter::Push(uint32_t slot) { Emit(OpCode::Push, slot); }
    void Emitter::Pop() { Emit(OpCode::Pop); }

    void Emitter::Copy(uint32_t source, uint32_t destination) {
        if (source == destination) return;
        Push(source);
        Push(destination);
        Emit(OpCode::Copy);
        Forget(destination);
    }

    void Emitter::CopyFromStack() {
        Emit(OpCode::Copy);
        reads_.clear();
    }

    void Emitter::Jump(Label target) {
        Emit(OpCode::Jump, 0xFFFFFFFCu);
        codeFixups_.emplace_back(code_.size() - 1, target.id);
    }

    void Emitter::JumpTo(uint32_t address) { Emit(OpCode::Jump, address); }

    void Emitter::JumpIfFalse(uint32_t condition, Label target) {
        Push(condition);
        Emit(OpCode::JumpIfFalse, 0xFFFFFFFCu);
        codeFixups_.emplace_back(code_.size() - 1, target.id);
    }

    void Emitter::JumpIndirect(uint32_t slot) { Emit(OpCode::JumpIndirect, slot); }

    void Emitter::Extern(uint32_t externSlot, bool lastPushIsResult, bool pure) {
        bool resultPushed = lastPushIsResult && code_.size() >= 2 && code_[code_.size() - 2] == static_cast<uint32_t>(OpCode::Push);
        size_t operand = code_.size() - 1;
        Emit(OpCode::Extern, externSlot);
        if (!pure || !resultPushed) reads_.clear();
        else Forget(code_[operand]);
        if (resultPushed) lastResultOperand_ = operand;
    }

    bool Emitter::RetargetLastResult(uint32_t from, uint32_t to) {
        if (!lastResultOperand_ || code_[*lastResultOperand_] != from) return false;
        if (slotTypes_[from] != slotTypes_[to]) return false;
        code_[*lastResultOperand_] = to;
        lastResultOperand_.reset();
        Forget(from);
        Forget(to);
        return true;
    }

    Program Emitter::Finish(std::vector<EntryPoint> entries, std::vector<SyncVariable> sync) {
        for (auto [index, label] : codeFixups_) {
            if (!labels_[label]) throw std::logic_error("unbound label");
            code_[index] = *labels_[label];
        }
        for (auto [slot, label] : slotFixups_) {
            if (!labels_[label]) throw std::logic_error("unbound label");
            heap_[slot].value.unsignedInteger = *labels_[label];
        }
        for (auto [slot, index, label] : elementFixups_) {
            if (!labels_[label]) throw std::logic_error("unbound label");
            heap_[slot].value.arguments[index].unsignedInteger = *labels_[label];
        }

        Program program;
        program.code = std::move(code_);
        program.heap = std::move(heap_);
        program.entryPoints = std::move(entries);
        program.sync = std::move(sync);
        std::ranges::stable_sort(lines_, {}, &LineEntry::address);
        program.lines = std::move(lines_);
        return program;
    }

} // namespace UdonLuau::Detail
