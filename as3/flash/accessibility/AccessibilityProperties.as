// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.accessibility {
    public class AccessibilityProperties {
        public var name:String;
        public var description:String;
        public var shortcut:String;
        public var silent:Boolean;
        public var forceSimple:Boolean;
        public var noAutoLabeling:Boolean;

        public function AccessibilityProperties() {
            this.name = "";
            this.description = "";
            this.shortcut = "";
            this.silent = false;
            this.forceSimple = false;
            this.noAutoLabeling = false;
        }
    }
}
