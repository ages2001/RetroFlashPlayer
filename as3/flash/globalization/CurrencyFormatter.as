// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.globalization {
    [API("667")]
    public final class CurrencyFormatter {
        private var _requestedLocaleIDName:String;
        private var _decimalSeparator:String = ".";
        private var _digitsType:uint = NationalDigitsType.EUROPEAN;
        private var _fractionalDigits:int = 2;
        private var _groupingPattern:String = "3;*";
        private var _groupingSeparator:String = ",";
        private var _leadingZero:Boolean = true;
        private var _negativeCurrencyFormat:uint = 1;
        private var _negativeSymbol:String = "-";
        private var _positiveCurrencyFormat:uint = 0;
        private var _trailingZeros = true;
        private var _useGrouping = true;

        public function CurrencyFormatter(requestedLocaleIDName:String) {
            this._requestedLocaleIDName = requestedLocaleIDName;
        }

        public function get actualLocaleIDName():String {
            return "en-US";
        }

        public function get currencyISOCode():String {
            return "USD";
        }

        public function get currencySymbol():String {
            return "$";
        }

        public function get decimalSeparator():String {
            return this._decimalSeparator;
        }

        public function set decimalSeparator(value:String):void {
            this._decimalSeparator = value;
        }

        public function get digitsType():uint {
            return this._digitsType;
        }

        public function set digitsType(value:uint):void {
            this._digitsType = value;
        }

        public function get fractionalDigits():int {
            return this._fractionalDigits;
        }

        public function set fractionalDigits(value:int):void {
            this._fractionalDigits = value;
        }

        public function get groupingPattern():String {
            return this._groupingPattern;
        }

        public function set groupingPattern(value:String):void {
            this._groupingPattern = value;
        }

        public function get groupingSeparator():String {
            return this._groupingSeparator;
        }

        public function set groupingSeparator(value:String):void {
            this._groupingSeparator = value;
        }

        public function get lastOperationStatus():String {
            return "noError";
        }

        public function get leadingZero():Boolean {
            return this._leadingZero;
        }

        public function set leadingZero(value:Boolean):void {
            this._leadingZero = value;
        }

        public function get negativeCurrencyFormat():uint {
            return this._negativeCurrencyFormat;
        }

        public function set negativeCurrencyFormat(value:uint):void {
            this._negativeCurrencyFormat = value;
        }

        public function get negativeSymbol():String {
            return this._negativeSymbol;
        }

        public function set negativeSymbol(value:String):void {
            this._negativeSymbol = value;
        }

        public function get positiveCurrencyFormat():uint {
            return this._positiveCurrencyFormat;
        }

        public function set positiveCurrencyFormat(value:uint):void {
            this._positiveCurrencyFormat = value;
        }

        public function get requestedLocaleIDName():String {
            return this._requestedLocaleIDName;
        }

        public function get trailingZeros():Boolean {
            return this._trailingZeros;
        }

        public function set trailingZeros(value:Boolean):void {
            this._trailingZeros = value;
        }

        public function get useGrouping():Boolean {
            return this._useGrouping;
        }

        public function set useGrouping(value:Boolean):void {
            this._useGrouping = value;
        }

        public function format(value:Number, withCurrencySymbol:Boolean = false):String {
            return value.toString();
        }

        public function formattingWithCurrencySymbolIsSafe(requestedISOCode:String):Boolean {
            return true;
        }

        public function parse(inputString:String):CurrencyParseResult {
            return new CurrencyParseResult();
        }

        public function setCurrency(currencyISOCode:String, currencySymbol:String):void {
        }

        public static function getAvailableLocaleIDNames():Vector.<String> {
            return new <String>["en-US"];
        }
    }
}
