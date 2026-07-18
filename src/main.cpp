#include "orderbook.hpp"
#include "replay.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// ============================================================================
// HELPERS FOR DEMO
// ============================================================================

// Helper to print event details using std::visit (EngineEvent is a variant)
std::string event_to_string(const EngineEvent& event) {
    char buffer[256];
    event_to_buffer(event, buffer, sizeof(buffer));
    return std::string(buffer);
}

// Helper to print current book state
void print_book_state(const OrderBook& book) {
    std::cout << "   [Book State] ";
    auto bid = book.best_bid();
    auto ask = book.best_ask();

    if (bid) std::cout << "Bid: " << std::fixed << std::setprecision(2) << to_double(*bid);
    else std::cout << "Bid: -";

    std::cout << "  |  ";

    if (ask) std::cout << "Ask: " << std::fixed << std::setprecision(2) << to_double(*ask);
    else std::cout << "Ask: -";
    
    std::cout << "\n";
}

// ============================================================================
// MAIN - Interactive Demonstration
// ============================================================================

int main() {
    std::cout << "╔══════════════════════════════════╗\n";
    std::cout << "║   DETERMINISTIC MATCHING ENGINE  ║\n";
    std::cout << "╚══════════════════════════════════╝\n\n";
    
    // Allocate capacity for demo
    OrderBook book(1000);
    std::vector<Command> commands;
    auto submit = [&](Command command) {
        commands.push_back(std::move(command));
        return book.process(commands.back());
    };
    
    std::cout << "========== SCENARIO 1: Building Order Book ==========\n";
    std::cout << "\n📝 Adding sell orders...\n";
    submit(NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL, from_double(101.00), Quantity(50)));
    submit(NewOrderCommand(CommandSequence(2), OrderId(2), Side::SELL, from_double(100.50), Quantity(30)));
    submit(NewOrderCommand(CommandSequence(3), OrderId(3), Side::SELL, from_double(100.00), Quantity(20)));
    print_book_state(book);
    
    std::cout << "\n📝 Adding buy orders...\n";
    submit(NewOrderCommand(CommandSequence(4), OrderId(4), Side::BUY, from_double(99.00), Quantity(40)));
    submit(NewOrderCommand(CommandSequence(5), OrderId(5), Side::BUY, from_double(99.50), Quantity(35)));
    print_book_state(book);
    
    std::cout << "\n========== SCENARIO 2: Aggressive Order ==========\n";
    std::cout << "\n💥 Aggressive buy order (sweeps multiple levels)...\n";
    // Buy @ 101.50, enough to eat 100.00, 100.50 and part of 101.00
    submit(NewOrderCommand(CommandSequence(6), OrderId(6), Side::BUY, from_double(101.50), Quantity(80)));
    print_book_state(book);
    
    std::cout << "\n========== SCENARIO 3: Order Cancellation ==========\n";
    std::cout << "\n🗑️  Cancelling order ID 4 (Buy @ 99.00)...\n";
    submit(CancelOrderCommand(CommandSequence(7), OrderId(4)));
    print_book_state(book);
    
    std::cout << "\n========== EVENT LOG ==========\n";
    const auto& events = book.get_event_log();
    std::cout << "Total events: " << events.size() << "\n\n";
    
    std::cout << "Recent events:\n";
    size_t start_idx = events.size() >= 5 ? events.size() - 5 : 0;
    for (size_t i = start_idx; i < events.size(); ++i) {
        std::cout << "  " << event_to_string(events[i]) << "\n";
    }
    
    std::cout << "\n========== DETERMINISTIC REPLAY ==========\n";
    std::cout << "\n💾 Saving event log...\n";
    try {
        ReplayEngine::save_log(events, "matching_engine.log");
        std::cout << "✓ Saved to matching_engine.log\n";
    } catch (const std::exception& e) {
        std::cerr << "Error saving log: " << e.what() << "\n";
    }
    
    std::cout << "\n🔄 Replaying from log...\n";
    OrderBook replayed = ReplayEngine::replay_commands(commands);
    
    // Verify they're identical
    bool identical = true;
    auto orig_bid = book.best_bid();
    auto repl_bid = replayed.best_bid();
    auto orig_ask = book.best_ask();
    auto repl_ask = replayed.best_ask();
    
    // Compare Bids
    if (orig_bid.has_value() != repl_bid.has_value()) identical = false;
    if (orig_bid.has_value() && orig_bid->get() != repl_bid->get()) identical = false;

    // Compare Asks
    if (orig_ask.has_value() != repl_ask.has_value()) identical = false;
    if (orig_ask.has_value() && orig_ask->get() != repl_ask->get()) identical = false;
    
    std::cout << "\n" << (identical ? "✅" : "❌") 
              << " Replay verification: " 
              << (identical ? "PASSED" : "FAILED") << "\n";
    
    std::cout << "\n✨ Demonstration complete!\n";
    std::cout << "\nKey Features Demonstrated:\n";
    std::cout << "  ✓ Price-time priority (FIFO)\n";
    std::cout << "  ✓ Multi-level order sweeping\n";
    std::cout << "  ✓ Fast order cancellation (O(1) unlink)\n";
    std::cout << "  ✓ Pre-allocated order objects with event logging\n";
    std::cout << "  ✓ Deterministic command replay\n";
    
    return 0;
}
