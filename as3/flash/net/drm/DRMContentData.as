// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.net.drm {
    import flash.utils.ByteArray;

    [API("667")]
    public class DRMContentData {
        public function DRMContentData(rawData:ByteArray = null) {
            super();
        }

        public function get serverURL():String {
            return null;
        }

        public function get authenticationMethod():String {
            return null;
        }

        public function get licenseID():String {
            return null;
        }

        public function get domain():String {
            return null;
        }

        public function getVoucherAccessInfo():Vector.<VoucherAccessInfo> {
            return null;
        }
    }
}
