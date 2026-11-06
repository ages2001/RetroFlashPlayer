// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.globalization {
    [API("667")]
    public final class DateTimeFormatter {
        private var _dateStyle:String;
        private var _dateTimePattern:String;
        private var _localeIDName:String;
        private var _timeStyle:String;

        private static function throwNonNull(name:String) {
            throw new TypeError("Error #2007: Parameter " + name + " must be non-null.", 2007);
        }

        public function DateTimeFormatter(requestedLocaleIDName:String, dateStyle:String = "long", timeStyle:String = "long") {
            if (requestedLocaleIDName == null) {
                throwNonNull("requestedLocaleIDName");
            }

            this._localeIDName = requestedLocaleIDName;
            this._dateTimePattern = "EEEE, MMMM d, yyyy h:mm:ss a";
            this.setDateTimeStyles(dateStyle, timeStyle);
        }

        public function get actualLocaleIDName():String {
            return this._localeIDName;
        }

        public function get lastOperationStatus():String {
            return LastOperationStatus.NO_ERROR;
        }

        public function get requestedLocaleIDName():String {
            return this._localeIDName;
        }

        public function format(dateTime:Date):String {
            if (dateTime == null) {
                throwNonNull("dateTime");
            }

            return dateTime.toString();
        }

        public function formatUTC(dateTime:Date):String {
            if (dateTime == null) {
                throwNonNull("dateTime");
            }

            return dateTime.toUTCString();
        }

        public static function getAvailableLocaleIDNames():Vector.<String> {
            return new <String>["en-US"];
        }

        public function getDateStyle():String {
            return this._dateStyle;
        }

        public function getDateTimePattern():String {
            return this._dateTimePattern;
        }

        public function getFirstWeekday():int {
            return 0;
        }

        public function getMonthNames(nameStyle:String = "full", context:String = "standalone"):Vector.<String> {
            if (nameStyle == null) {
                throwNonNull("nameStyle");
            }

            if (context == null) {
                throwNonNull("context");
            }

            return new <String>[
                "January",
                "February",
                "March",
                "April",
                "May",
                "June",
                "July",
                "August",
                "September",
                "October",
                "November",
                "December"
            ];
        }

        public function getTimeStyle():String {
            return this._timeStyle;
        }

        public function getWeekdayNames(nameStyle:String = "full", context:String = "standalone"):Vector.<String> {
            if (nameStyle == null) {
                throwNonNull("nameStyle");
            }

            if (context == null) {
                throwNonNull("context");
            }

            return new <String>["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
        }

        public function setDateTimePattern(pattern:String):void {
            if (pattern == null) {
                throwNonNull("pattern");
            }

            this._dateTimePattern = pattern;
        }

        public function setDateTimeStyles(dateStyle:String, timeStyle:String):void {
            if (dateStyle == null) {
                throwNonNull("dateStyle");
            }

            if (timeStyle == null) {
                throwNonNull("timeStyle");
            }

            this._dateStyle = dateStyle;
            this._timeStyle = timeStyle;
        }
    }
}
