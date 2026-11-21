# EventBus finds @Subscribe methods by reflection (BleProvisioningPlugin
# subscribes to the provisioning library's DeviceConnectionEvent).
-keepattributes *Annotation*
-keepclassmembers class * {
    @org.greenrobot.eventbus.Subscribe <methods>;
}
-keep enum org.greenrobot.eventbus.ThreadMode { *; }
