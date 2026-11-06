// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.media {
    import flash.events.EventDispatcher;

    [API("669")]
    public final class StageWebView extends EventDispatcher {
        // NOTE: Before AIR 51, the constructor was
        // `StageWebView(useNative:Boolean = false, mediaPlaybackRequiresUserAction:Boolean = true)`
        public function StageWebView(...configuration) {
        }
    }
}
