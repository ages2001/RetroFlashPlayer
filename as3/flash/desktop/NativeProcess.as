// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.desktop {
    import flash.events.EventDispatcher;

    [API("668")]
    public class NativeProcess extends EventDispatcher {
        public function NativeProcess() {
            super();
        }

        public static function get isSupported():Boolean {
            return false;
        }

        public function start(info:NativeProcessStartupInfo):void {
        }
    }
}
