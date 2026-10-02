#pragma once

#include "TypeSystem.hpp"
#include "UdonLuau/Program.hpp"

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace UdonLuau::Detail {

    struct Label {
        int id = -1;
    };

    class Emitter {
    public:
        uint32_t AddSlot(std::string symbol, const Type* type, HeapValue value = {}, bool exported = false);
        uint32_t Constant(const Type* type, const HeapValue& value);
        uint32_t ExternSlot(const std::string& signature);
        uint32_t AddressConstant(Label target);
        uint32_t Local(std::string_view name, const Type* type);
        uint32_t Hidden(const Type* type);

        uint32_t JumpTable(const Type* arrayType, const Type* elementType, const std::vector<Label>& targets);

        uint32_t Temp(const Type* type);
        [[nodiscard]] size_t TempMark() const { return liveTemps_.size(); }
        void ReleaseTemps(size_t mark);
        void Promote(uint32_t slot);
        void ResetTemps();
        [[nodiscard]] bool IsTemp(uint32_t slot) const;
        void MarkConstant(uint32_t slot);
        [[nodiscard]] bool IsConstant(uint32_t slot) const;

        [[nodiscard]] Label NewLabel();
        void Bind(Label label);

        void Push(uint32_t slot);
        void Pop();
        void Copy(uint32_t source, uint32_t destination);
        void CopyFromStack();
        void Jump(Label target);
        void JumpTo(uint32_t address);
        void JumpIfFalse(uint32_t condition, Label target);
        void JumpIndirect(uint32_t slot);
        void Extern(uint32_t externSlot, bool lastPushIsResult, bool pure = false);

        [[nodiscard]] std::optional<uint32_t> CachedRead(uint32_t externSlot, uint32_t receiver) const;
        void RememberRead(uint32_t externSlot, uint32_t receiver, uint32_t result);

        bool RetargetLastResult(uint32_t from, uint32_t to);

        [[nodiscard]] const HeapSlot& Slot(uint32_t address) const { return heap_[address]; }
        [[nodiscard]] HeapSlot& Slot(uint32_t address) { return heap_[address]; }
        [[nodiscard]] uint32_t Here() const { return static_cast<uint32_t>(code_.size() * 4); }
        [[nodiscard]] std::optional<uint32_t> AddressOf(Label label) const;

        [[nodiscard]] Program Finish(std::vector<EntryPoint> entries, std::vector<SyncVariable> sync);

    private:
        void Emit(OpCode op);
        void Emit(OpCode op, uint32_t operand);
        std::string Unique(std::string base);
        void Forget(uint32_t slot);

        std::vector<uint32_t>                       code_;
        std::vector<HeapSlot>                       heap_;
        std::vector<const Type*>                    slotTypes_;
        std::map<std::string, uint32_t>             constants_;
        std::unordered_map<std::string, uint32_t>   externs_;
        std::unordered_map<std::string, int>        nameCounters_;
        std::unordered_map<const Type*, std::vector<uint32_t>> freeTemps_;
        std::vector<uint32_t>                       liveTemps_;
        std::vector<bool>                           isTemp_;
        std::vector<bool>                           isConstant_;
        std::vector<std::tuple<uint32_t, size_t, int>> elementFixups_;
        std::vector<std::optional<uint32_t>>        labels_;
        std::vector<std::pair<size_t, int>>         codeFixups_;
        std::vector<std::pair<uint32_t, int>>       slotFixups_;
        std::optional<size_t>                       lastResultOperand_;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> reads_;
    };

} // namespace UdonLuau::Detail
