// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.globalization {
    [API("667")]
    public final class StringTools {
        private var _requestedLocaleIDName:String;

        public function StringTools(requestedLocaleIDName:String) {
            this._requestedLocaleIDName = requestedLocaleIDName;
        }

        public function get actualLocaleIDName():String {
            return "en-US";
        }

        public function get lastOperationStatus():String {
            return "noError";
        }

        public function get requestedLocaleIDName():String {
            return this._requestedLocaleIDName;
        }

        public function toLowerCase(s:String):String {
            return s.toLowerCase();
        }

        public function toUpperCase(s:String):String {
            return s.toUpperCase();
        }

        public static function getAvailableLocaleIDNames():Vector.<String> {
            return new <String>["en-US"];
        }
    }
}
