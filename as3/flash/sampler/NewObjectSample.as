// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.sampler {
    public final class NewObjectSample extends Sample {
        public const id:Number;
        public const type:Class;

        public function get object():* {
            return {};
        }

        public function get size():Number {
            return 0;
        }
    }
}
