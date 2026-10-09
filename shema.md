Control Manager – orchestrates/supervises the other services (startup, health, config, lifecycle).
MD Provider – connects to exchanges (Binance/OKX/Bybit), normalizes market data into core domain messages.
SymbolTracker – holds symbol/instrument metadata and reference data shared across services.
Candle Manager – builds and serves OHLC candles from market data.

## Control Manager startup flow

1. Run Control Manager.
2. Get symbols (vector of strings).
3. Open websocket / open subscription.

using Symbol = std::string;

#include "types/candle.h"

enum TimeFrame{
     1m...., 1h, ...1d
}

class SymbolTracker{
    TimeFrame timeframe;
    VenueId venueId; 
    EMAManager emaManager;
    deque<Candle> last_n_candles; //store last 10 candles
    bool is_valid; // once sybol warmUp
    void WarmUp() { REST GEt history from exchange
     apply hostory + apply candles from  warmup_vec_
     is_valid = true;
     clean warmup_vec_;
    }
    atd::vector<Candle> warmup_vec_;
    void ApplyUpdate(){
        if not warm up -> add into warmup_vec_;
        else append last_n_candles;
        apply EMAManager;
    }
};

//eacj stream contains a list a banch of symbols ( assume we have 1000)
// we have symbol_limit 
so new stream qnty = rest / symbol_limit ? symbol_limit : rest;
MDProvider{
    RunStream;
    CallBack() -> push message throw ApplyUpdate()
}


class CandleManager{
Symbol symbol;
  SymbolTracker 1h_tracker; 
  SymbolTracker 4h_tracker;
  void ApplyUpdate(TimeFrame tf) {
    1h -> 1h_tracker.ApplyUpdate();
    4h -> 4h_tracker.ApplyUpdate();
  }
};




class CoreManager{

 public:

 private:
   unorderd_map<Symbol,  CandleManager> tracked_instrumnets_;
    unorderd_map<Symbol,  CandleManager> active_instruments_;

    vector<MDProvider> md_providers; // we assign all tracked_instrumnets_;
    


    void GetSymbol() { return array of instuments}
    void Run() 

    void ApplyUpdate(){
        // parse message 
        // find Instrument

    }

    void TryDeactivate(Symbol instrument){
        //go throw active_instruments_, if condition is deactive -> change to deactive and remove from active_instruments_ 
        Notify ( symbol is deactive)
    }

    void TryActivate(Symbol instrument){
        //if condition is deactive -> change to activate, 
        add to active_instruments_ 
        Notify ( symbol is Active);
    }
}