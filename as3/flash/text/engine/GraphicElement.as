// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.text.engine {
    import flash.display.DisplayObject;
    import flash.events.EventDispatcher;

    [API("662")]
    public final class GraphicElement extends ContentElement {
        public function GraphicElement(
            graphic:DisplayObject = null,
            elementWidth:Number = 15.0,
            elementHeight:Number = 15.0,
            elementFormat:ElementFormat = null,
            eventMirror:EventDispatcher = null,
            textRotation:String = "rotate0"
        ) {
            super(elementFormat, eventMirror, textRotation);
            this.init();
        }

        private native function init():void;

        public function get elementHeight():Number {
            return 15.0;
        }
        public function set elementHeight(value:Number):void {
        }

        public function get elementWidth():Number {
            return 15.0;
        }

        public function set elementWidth(value:Number):void {
        }

        public function get graphic():DisplayObject {
            return null;
        }

        public function set graphic(value:DisplayObject):void {
        }
    }
}
