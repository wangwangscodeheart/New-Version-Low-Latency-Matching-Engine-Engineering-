#ifndef ORDER_HPP
#define ORDER_HPP

#include "types.hpp"
#include <vector>
#include <cstddef>

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
    size_t capacity_;

public:
    explicit ObjectPool(size_t capacity) : capacity_(capacity) {
        pool_.reserve(capacity);
        free_list_.reserve(capacity);

        // Pre-allocate all objects
        for (size_t i = 0; i < capacity; ++i) {
            pool_.emplace_back(OrderId(0), PrioritySequence(0), Side::BUY,
                              Price(0), Quantity(0));
            free_list_.push_back(&pool_[i]);
        }
    }

    T* allocate() {
        if (free_list_.empty()) {
            return nullptr;  // Pool exhausted
        }
        T* obj = free_list_.back();
        free_list_.pop_back();
        return obj;
    }
    
    void deallocate(T* obj) {
        if (obj) {
            free_list_.push_back(obj);
        }
    }
    
    size_t available() const {
        return free_list_.size();
    }

    size_t capacity() const {
        return capacity_;
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
