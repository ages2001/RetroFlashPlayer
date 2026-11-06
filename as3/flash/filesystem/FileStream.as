// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.filesystem {
    import flash.events.EventDispatcher;

    // TODO this class should implement IDataInput and IDataOutput

    [API("661")]
    public class FileStream extends EventDispatcher {
        public function FileStream() {
            super();
        }

        public function close():void {
        }
    }
}
