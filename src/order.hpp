#ifndef ORDER_HPP
#define ORDER_HPP

#include "types.hpp"
#include <vector>
#include <cstddef>
#include <cstdint>
#include <utility>

// ============================================================================
// ORDER - Cache-aligned order with intrusive list pointers
// ============================================================================

struct LimitLevel;

struct Order {
    OrderId id;
    PrioritySequence priority_sequence;
    Side side;
    Price price;
    Quantity original_qty;
    Quantity remaining_qty;
    
    // Intrusive list pointers
    Order* next;
    Order* prev;
    LimitLevel* parent_level;
    
    Order(OrderId id_, PrioritySequence priority, Side s, Price p, Quantity q)
        : id(id_), priority_sequence(priority), side(s), price(p),
          original_qty(q), remaining_qty(q), next(nullptr), prev(nullptr), parent_level(nullptr) {}

    void activate(OrderId id_, PrioritySequence priority, Side s, Price p, Quantity q) noexcept {
        id = id_;
        priority_sequence = priority;
        side = s;
        price = p;
        original_qty = q;
        remaining_qty = q;
        next = nullptr;
        prev = nullptr;
        parent_level = nullptr;
    }

    void activate_from_snapshot(OrderId id_, PrioritySequence priority, Side s, Price p,
                                Quantity original, Quantity remaining) noexcept {
        id = id_;
        priority_sequence = priority;
        side = s;
        price = p;
        original_qty = original;
        remaining_qty = remaining;
        next = nullptr;
        prev = nullptr;
        parent_level = nullptr;
    }
    
    bool is_filled() const { 
        return remaining_qty.get() == 0; 
    }
    
    bool check_invariants() const {
        return remaining_qty.get() <= original_qty.get();
    }
};

// ============================================================================
// OBJECT POOL - Pre-allocated memory pool for orders
// ============================================================================

template<typename T>
class ObjectPool {
private:
    std::vector<T> pool_;
    std::vector<T*> free_list_;
    std::vector<uint8_t> in_use_;

public:
    explicit ObjectPool(size_t capacity) : in_use_(capacity, 0) {
        pool_.reserve(capacity);
        free_list_.reserve(capacity);

        // Pre-allocate all objects
        for (size_t i = 0; i < capacity; ++i) {
            pool_.emplace_back(OrderId(0), PrioritySequence(0), Side::BUY,
                              Price(0), Quantity(0));
            free_list_.push_back(&pool_[i]);
        }
    }

    // A copied free list would still point into the source pool's storage.
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&& other) noexcept
        : pool_(std::move(other.pool_)), free_list_(std::move(other.free_list_)),
          in_use_(std::move(other.in_use_)) {
        other.pool_.clear();
        other.free_list_.clear();
        other.in_use_.clear();
    }
    ObjectPool& operator=(ObjectPool&&) = delete;

    T* allocate() noexcept {
        if (free_list_.empty()) {
            return nullptr;  // Pool exhausted
        }
        T* obj = free_list_.back();
        free_list_.pop_back();
        // The free list contains only canonical pointers owned by this pool.
        in_use_[static_cast<size_t>(obj - pool_.data())] = 1;
        return obj;
    }
    
    void deallocate(T* obj) noexcept {
        if (!obj || pool_.empty()) return;
        // Supported targets use a flat uintptr_t address representation. Never
        // subtract unrelated pointers or dereference an unchecked candidate.
        const uintptr_t base = reinterpret_cast<uintptr_t>(pool_.data());
        const uintptr_t address = reinterpret_cast<uintptr_t>(obj);
        if (address < base) return;
        const uintptr_t offset = address - base;
        if (offset % sizeof(T) != 0) return;
        const uintptr_t index = offset / sizeof(T);
        if (index >= pool_.size() || &pool_[index] != obj || !in_use_[index]) return;
        in_use_[index] = 0;
        free_list_.push_back(obj); // At most capacity entries; never grows here.
    }
    
    size_t available() const noexcept {
        return free_list_.size();
    }

    size_t capacity() const noexcept {
        return pool_.size();
    }

};

// ============================================================================
// LIMIT LEVEL - Intrusive doubly-linked list
// ============================================================================

struct LimitLevel {
    Price price;
    Order* head;
    Order* tail;
    Quantity total_volume;
    size_t order_count;
    
    LimitLevel() : LimitLevel(Price(0)) {}

    explicit LimitLevel(Price p) 
        : price(p), head(nullptr), tail(nullptr), 
          total_volume(Quantity(0)), order_count(0) {}
    
    void add_order(Order* order) {
        order->next = nullptr;
        order->prev = tail;
        order->parent_level = this;
        
        if (tail) {
            tail->next = order;
        } else {
            head = order;
        }
        
        tail = order;
        total_volume = Quantity(total_volume.get() + order->remaining_qty.get());
        ++order_count;
    }
    
    Order* front() { 
        return head; 
    }
    
    void pop() {
        if (!head) return;

        Order* removed = head;
        
        head = head->next;
        
        if (head) {
            head->prev = nullptr;
        } else {
            tail = nullptr;
        }

        removed->next = nullptr;
        removed->prev = nullptr;
        removed->parent_level = nullptr;
        
        --order_count;
    }
    
    bool empty() const { 
        return head == nullptr; 
    }
    
    size_t size() const { 
        return order_count; 
    }
    
    bool check_invariants() const {
        if (empty()) {
            return total_volume.get() == 0 && order_count == 0;
        }
        
        uint64_t computed_volume = 0;
        size_t computed_count = 0;
        Order* curr = head;
        
        while (curr) {
            if (!curr->check_invariants()) return false;
            computed_volume += curr->remaining_qty.get();
            ++computed_count;
            curr = curr->next;
        }
        
        return computed_volume == total_volume.get() && 
               computed_count == order_count;
    }
};

#endif
