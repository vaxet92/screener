

struct Candle {
    ""
    "Represents a single candlestick bar
        Mirrors the Pine Script Candle type structure
    ""
    "
        open : float high : float low : float close : float index : int =
                   0 volume : float = 0.0 timestamp : int = 0 #Unix seconds UTC(bar open time);

#def is_bullish(self)->bool:
#""                                                              \
 "Returns True if candle closed higher than it opened (bullish)" \
 ""
#return self.close> self.open

#def range(self)->float:
#""                                     \
 "Returns the total range (high - low)" \
 ""
#return self.high - self.low

#def __str__(self)->str:
#""                                              \
 "Return a string representation of the candle." \
 ""
#direction = "🟢" if self.is_bullish() else "🔴"
#return f "Candle[{self.index}] {direction} O:{self.open} H:{self.high} L:{self.low} C:{self.close}"

#def __repr__(self)->str:
#""                                              \
 "Return a string representation of the candle." \
 ""
#return self.__str__()
}

enum class CandleType {
    : PIN_BAR = 0 BULLISH = 1 BEARISH = 2
}
