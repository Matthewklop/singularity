/* ============================================================================
 * OraclePatcherV8.java — Transforms Vec3i, BlockPos, ItemEntity, Hopper
 *
 * Vec3i:     int x,y,z → short (parent of BlockPos — this is the real target)
 * BlockPos:  inherits from Vec3i, already covered
 * ItemEntity: age, pickupDelay → int→short
 * HopperBlockEntity: cooldownTime → int→short
 * ============================================================================
 */
import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.Instrumentation;
import java.security.ProtectionDomain;
import org.objectweb.asm.*;

public class OraclePatcherV8 {
    static int totalPatched = 0;

    public static void premain(String args, Instrumentation inst) {
        System.out.println("[OraclePatcher] v8 REAL — transforming core classes");
        inst.addTransformer(new OracleTransformer(), false);
    }

    static class OracleTransformer implements ClassFileTransformer {
        public byte[] transform(ClassLoader loader, String className,
                                Class<?> classBeingRedefined,
                                ProtectionDomain protectionDomain,
                                byte[] classfileBuffer) {
            if (className == null) return null;

            /* Only target specific classes */
            boolean target = className.equals("net/minecraft/core/Vec3i") ||
                             className.equals("net/minecraft/core/BlockPos") ||
                             className.equals("net/minecraft/world/entity/item/ItemEntity") ||
                             className.equals("net/minecraft/world/level/block/entity/HopperBlockEntity");
            if (!target) return null;

            try {
                ClassReader cr = new ClassReader(classfileBuffer);
                ClassWriter cw = new ClassWriter(cr, ClassWriter.COMPUTE_MAXS);

                cr.accept(new ClassVisitor(Opcodes.ASM9, cw) {
                    @Override
                    public FieldVisitor visitField(int access, String name, String descriptor, String signature, Object value) {
                        String newDesc = descriptor;

                        /* Vec3i: int x,y,z → short */
                        if (className.equals("net/minecraft/core/Vec3i") && descriptor.equals("I") &&
                            (name.equals("x") || name.equals("y") || name.equals("z"))) {
                            newDesc = "S"; System.out.println("[OraclePatcher] Vec3i." + name + ": int→short");
                        }

                        /* ItemEntity: int age, pickupDelay → short */
                        if (className.equals("net/minecraft/world/entity/item/ItemEntity") && descriptor.equals("I") &&
                            (name.equals("age") || name.equals("pickupDelay"))) {
                            newDesc = "S"; System.out.println("[OraclePatcher] ItemEntity." + name + ": int→short");
                        }

                        /* HopperBlockEntity: int cooldownTime → short */
                        if (className.equals("net/minecraft/world/level/block/entity/HopperBlockEntity") && descriptor.equals("I") &&
                            name.equals("cooldownTime")) {
                            newDesc = "S"; System.out.println("[OraclePatcher] Hopper." + name + ": int→short");
                        }

                        return super.visitField(access, name, newDesc, signature, value);
                    }

                    @Override
                    public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
                        MethodVisitor mv = super.visitMethod(access, name, descriptor, signature, exceptions);
                        return new MethodVisitor(Opcodes.ASM9, mv) {
                            @Override
                            public void visitFieldInsn(int opcode, String owner, String fname, String fdesc) {
                                String newDesc = fdesc;
                                /* Insert I2S/S2I conversions where needed */
                                if (fdesc.equals("I") && opcode == Opcodes.GETFIELD &&
                                    isTargetField(owner, fname)) {
                                    newDesc = "S";
                                    mv.visitInsn(Opcodes.I2S);
                                }
                                if (fdesc.equals("I") && opcode == Opcodes.PUTFIELD &&
                                    isTargetField(owner, fname)) {
                                    newDesc = "S";
                                    mv.visitInsn(Opcodes.I2S);
                                }
                                super.visitFieldInsn(opcode, owner, fname, newDesc);
                            }
                        };
                    }
                }, 0);

                byte[] result = cw.toByteArray();
                totalPatched++;
                System.out.println("[OraclePatcher] ✓ " + className + " transformed");
                return result;
            } catch (Exception e) {
                System.out.println("[OraclePatcher] ERROR: " + className + " — " + e.getMessage());
                return null;
            }
        }

        static boolean isTargetField(String owner, String name) {
            if (owner.equals("net/minecraft/core/Vec3i") && (name.equals("x")||name.equals("y")||name.equals("z"))) return true;
            if (owner.equals("net/minecraft/world/entity/item/ItemEntity") && (name.equals("age")||name.equals("pickupDelay"))) return true;
            if (owner.equals("net/minecraft/world/level/block/entity/HopperBlockEntity") && name.equals("cooldownTime")) return true;
            return false;
        }
    }
}
