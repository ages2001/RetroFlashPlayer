// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.globalization {
    import flash.globalization.LastOperationStatus;

    [API("667")]
    public final class LocaleID {
        public static const DEFAULT:String = "i-default";

        private var _name:String;

        public function LocaleID(name:String) {
            this._name = name;
        }

        public function get lastOperationStatus():String {
            return LastOperationStatus.NO_ERROR;
        }

        public function get name():String {
            return this._name;
        }

        public static function determinePreferredLocales(
            want:Vector.<String>,
            have:Vector.<String>,
            keyword:String = "userinterface"
        ):Vector.<String> {
            return null;
        }

        public function getKeysAndValues():Object {
            return new Object();
        }

        public function getLanguage():String {
            return this._name;
        }

        public function getRegion():String {
            return "";
        }

        public function getScript():String {
            return "";
        }

        public function getVariant():String {
            return "";
        }

        public function isRightToLeft():Boolean {
            return false;
        }
    }
}
