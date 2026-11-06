// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package flash.net {
    import flash.net.NetConnection;
    import flash.net.NetStreamPlayOptions;
    import flash.net.NetStreamInfo;
    import flash.events.EventDispatcher;
    import flash.utils.ByteArray;
    import flash.media.Microphone;
    import flash.media.Camera;
    import flash.media.VideoStreamSettings;

    [Ruffle(InstanceAllocator)]
    public class NetStream extends EventDispatcher {
        public static const CONNECT_TO_FMS:String = "connectToFMS";
        public static const DIRECT_CONNECTIONS:String = "directConnections";

        public function NetStream(connection:NetConnection, peer:String = CONNECT_TO_FMS) {}

        public function appendBytes(bytes:ByteArray) {
        }

        public function appendBytesAction(action:String) {
        }

        public function attach(connection:NetConnection) {
        }

        public function attachAudio(mic:Microphone) {
        }

        public function attachCamera(cam:Camera, ms:int = -1) {
        }

        public function close() {
        }

        [API("674")]
        public function dispose() {
        }

        public native function pause();

        public native function play(...args);

        public function play2(param:NetStreamPlayOptions) {
        }

        [API("663")]
        public function preloadEmbeddedData(param:NetStreamPlayOptions) {
        }

        public function publish(name:String=null, type:String=null) {
        }

        public function receiveAudio(flag:Boolean) {
        }

        public function receiveVideo(flag:Boolean) {
        }

        public function receiveVideoFPS(fps:Number) {
        }

        [API("690")]
        public static function resetDRMVouchers() {
        }

        public native function resume();

        public native function seek(offset:Number);

        public function send(handlerName:String, ...args) {
        }

        [API("661")]
        public function setDRMAuthenticationCredentials(userName:String, password:String, type:String) {
        }

        public function step(frames:int) {
        }

        public native function togglePause();

        public function get audioReliable():Boolean {
            return false;
        }

        public function set audioReliable(isReliable:Boolean) {
        }

        public function get audioSampleAccess():Boolean {
            return false;
        }

        public function set audioSampleAccess(isAccessible:Boolean) {
        }

        public function get backBufferLength():Number {
            return 0.0;
        }

        public function get backBufferTime():Number {
            return 0.0;
        }

        public function set backBufferTime(time:Number) {
        }

        public function get bufferLength():Number {
            return 0.0;
        }

        public function get bufferTime():Number {
            return 0.0;
        }

        public function set bufferTime(time:Number) {
        }

        public function get bufferTimeMax():Number {
            return 0.0;
        }

        public function set bufferTimeMax(time:Number) {
        }

        public native function get bytesLoaded():uint;

        public native function get bytesTotal():uint;

        public function get checkPolicyFile():Boolean {
            return false;
        }

        public function set checkPolicyFile(doCheck:Boolean) {
        }

        public native function get client():Object;

        public native function set client(client:Object);

        public function get currentFPS():Number {
            return 0.0;
        }

        public function get dataReliable():Boolean {
            return false;
        }

        public function set dataReliable(isReliable:Boolean) {
        }

        public function get farID():String {
            return "";
        }

        public function get farNonce():String {
            return "";
        }

        public function get inBufferSeek():Boolean {
            return false;
        }

        public function set inBufferSeek(isInBuffer:Boolean) {
        }

        public function get info():NetStreamInfo {
            return new NetStreamInfo();
        }

        public function get liveDelay():Number {
            return 0;
        }

        public function get maxPauseBufferTime():Number {
            return 0;
        }

        public function set maxPauseBufferTime(time:Number) {
        }

        public function get multicastAvailabilitySendToAll():Boolean {
            return false;
        }

        public function set multicastAvailabilitySendToAll(toAll:Boolean) {
        }

        public function get multicastAvailabilityUpdatePeriod():Number {
            return 0;
        }

        public function set multicastAvailabilityUpdatePeriod(period:Number) {
        }

        public function get multicastFetchPeriod():Number {
            return 0;
        }

        public function set multicastFetchPeriod(period:Number) {
        }

        public function get multicastInfo():NetStreamMulticastInfo {
            return new NetStreamMulticastInfo();
        }

        public function get multicastPushNeighborLimit():Number {
            return 0;
        }

        public function set multicastPushNeighborLimit(limit:Number) {
        }

        public function get multicastRelayMarginDuration():Number {
            return 0;
        }

        public function set multicastRelayMarginDuration(dur:Number) {
        }

        public function get multicastWindowDuration():Number {
            return 0;
        }

        public function set multicastWindowDuration(dur:Number) {
        }

        public function get nearNonce():String {
            return "";
        }

        public function get objectEncoding():uint {
            return 0;
        }

        public function get peerStreams():Array {
            return [];
        }

        public function get soundTransform():flash.media.SoundTransform {
            return new flash.media.SoundTransform();
        }

        public function set soundTransform(tf:flash.media.SoundTransform) {
        }

        public native function get time():Number;

        public function get useHardwareDecoder():Boolean {
            return true;
        }

        public function set useHardwareDecoder(dec:Boolean) {
        }

        [API("680")]
        public function get useJitterBuffer():Boolean {
            return false;
        }

        [API("680")]
        public function set useJitterBuffer(jbuf:Boolean) {
        }

        public function get videoReliable():Boolean {
            return false;
        }

        public function set videoReliable(isReliable:Boolean) {
        }

        public function get videoSampleAccess():Boolean {
            return false;
        }

        public function set videoSampleAccess(isReadable:Boolean) {
        }

        [API("674")]
        public function get videoStreamSettings():VideoStreamSettings {
            return null;
        }

        [API("674")]
        public function set videoStreamSettings(settings:VideoStreamSettings) {
        }
    }
}
