// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.
package avm2.intrinsics.memory {
    public native function mfence():void;

    public native function casi32(address:int, expectedValue:int, newValue:int):int;
}
