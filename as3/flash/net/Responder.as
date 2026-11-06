// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.net {
[Ruffle(InstanceAllocator)]
    public class Responder {
        public function Responder(result:Function, status:Function = null) {
            init(result, status);
        }

        private native function init(result:Function, status:Function = null):void;
    }
}
