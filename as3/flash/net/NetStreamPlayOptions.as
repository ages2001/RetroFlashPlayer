// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.net {
    import flash.events.EventDispatcher;

    public dynamic class NetStreamPlayOptions extends EventDispatcher {
        public var len:Number = -1;
        public var offset:Number = -1;
        public var oldStreamName:String;
        public var start:Number = -2;
        public var streamName:String;
        public var transition:String;
    }
}
