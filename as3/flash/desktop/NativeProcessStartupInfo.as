// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.desktop {
    import flash.filesystem.File;

    [API("668")]
    public class NativeProcessStartupInfo {
        public function NativeProcessStartupInfo() {
            super();
        }

        public function get arguments():Vector.<String> {
            return null;
        }

        public function set arguments(value:Vector.<String>):void {
        }

        public function get executable():File {
            return null;
        }

        public function set executable(value:File):void {
        }

        public function get workingDirectory():File {
            return null;
        }

        public function set workingDirectory(value:File):void {
        }
    }
}
