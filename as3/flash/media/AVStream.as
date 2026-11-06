// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.media {
    import flash.events.EventDispatcher;

    [API("688")]
    public class AVStream extends EventDispatcher {
        public static const HARDWARE:String = "hardware";
        // Note: This is not a typo.
        public static const SOFTWARE:String = "sofware";
        public static const UNDEFINED:String = "undefined";

        public function AVStream(source:AVSource) {
        }
    }
}
