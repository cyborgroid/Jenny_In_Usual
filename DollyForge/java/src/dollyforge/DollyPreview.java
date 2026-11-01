package dollyforge;

import java.awt.BasicStroke;
import java.awt.Color;
import java.awt.Font;
import java.awt.FontMetrics;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.geom.Path2D;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import javax.imageio.ImageIO;

/**
 * Java Edition 플레이어 모델로 스킨을 3D(정사영) 렌더한다. 외부 라이브러리 없이 Java2D 만 쓴다.
 *
 * <pre>
 * java -cp build/java dollyforge.DollyPreview render skin.png out.png [--scale 10]
 * java -cp build/java dollyforge.DollyPreview lineup out.png a.png:"Title A" b.png:"Title B" ...
 * </pre>
 */
public final class DollyPreview {

    /** 텍스처 좌표계(libdollyforge 와 동일한 64x64 규격) 위의 박스 하나 */
    record Box(String part, boolean overlay, double x0, double y0, double z0,
               double x1, double y1, double z1, int u, int v, int w, int h, int d) {}

    /** 화면에 그릴 텍셀 하나 */
    record Quad(double[] xs, double[] ys, double depth, int argb) {}

    private static final double[] LIGHT = normalize(new double[] {-0.3, 0.6, 0.75});

    private DollyPreview() {}

    // ---- 모델 ----

    static List<Box> model(boolean slim) {
        int aw = slim ? 3 : 4;
        List<Box> boxes = new ArrayList<>();
        // part, 원점 x/y/z 범위, base 텍스처 원점, overlay 텍스처 원점, 크기, 부풀림
        addPart(boxes, "head", -4, 24, -4, 4, 32, 4, 0, 0, 32, 0, 8, 8, 8, 0.5);
        addPart(boxes, "body", -4, 12, -2, 4, 24, 2, 16, 16, 16, 32, 8, 12, 4, 0.25);
        addPart(boxes, "arm_r", -4 - aw, 12, -2, -4, 24, 2, 40, 16, 40, 32, aw, 12, 4, 0.25);
        addPart(boxes, "arm_l", 4, 12, -2, 4 + aw, 24, 2, 32, 48, 48, 48, aw, 12, 4, 0.25);
        addPart(boxes, "leg_r", -4, 0, -2, 0, 12, 2, 0, 16, 0, 32, 4, 12, 4, 0.25);
        addPart(boxes, "leg_l", 0, 0, -2, 4, 12, 2, 16, 48, 0, 48, 4, 12, 4, 0.25);
        return boxes;
    }

    private static void addPart(List<Box> boxes, String part, double x0, double y0, double z0,
                                double x1, double y1, double z1, int bu, int bv, int ou, int ov,
                                int w, int h, int d, double inflate) {
        boxes.add(new Box(part, false, x0, y0, z0, x1, y1, z1, bu, bv, w, h, d));
        boxes.add(new Box(part, true, x0 - inflate, y0 - inflate, z0 - inflate,
                x1 + inflate, y1 + inflate, z1 + inflate, ou, ov, w, h, d));
    }

    /** 64x64 의 slim 여부: 오른팔의 x=54..55, y=20..31 열이 비어 있으면 slim (C 코어와 같은 규칙) */
    static boolean detectSlim(BufferedImage skin) {
        for (int y = 16; y < 32; y++) {
            int from = y < 20 ? 50 : 54;
            for (int x = from; x < from + 2; x++) {
                if ((skin.getRGB(x, y) >>> 24) != 0) return false;
            }
        }
        return true;
    }

    // ---- 렌더 ----

    /** yaw: 0 = 정면, 180 = 후면. 양수면 캐릭터 오른쪽 옆면이 보인다. */
    static BufferedImage render(BufferedImage skin, double yawDeg, double pitchDeg, int scale) {
        boolean slim = detectSlim(skin);
        double yaw = Math.toRadians(yawDeg), pitch = Math.toRadians(pitchDeg);
        List<Quad> quads = new ArrayList<>();
        for (Box b : model(slim)) emitBox(skin, b, yaw, pitch, quads);
        quads.sort((a, c) -> Double.compare(a.depth(), c.depth()));

        double minX = Double.MAX_VALUE, minY = Double.MAX_VALUE, maxX = -Double.MAX_VALUE, maxY = -Double.MAX_VALUE;
        for (Quad q : quads) {
            for (int i = 0; i < 4; i++) {
                minX = Math.min(minX, q.xs()[i]); maxX = Math.max(maxX, q.xs()[i]);
                minY = Math.min(minY, q.ys()[i]); maxY = Math.max(maxY, q.ys()[i]);
            }
        }
        int pad = scale * 2;
        int w = (int) Math.ceil((maxX - minX) * scale) + pad * 2;
        int h = (int) Math.ceil((maxY - minY) * scale) + pad * 2;
        BufferedImage out = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = out.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);
        g.setStroke(new BasicStroke(1.0f));
        for (Quad q : quads) {
            Path2D.Double p = new Path2D.Double();
            for (int i = 0; i < 4; i++) {
                double sx = (q.xs()[i] - minX) * scale + pad, sy = (q.ys()[i] - minY) * scale + pad;
                if (i == 0) p.moveTo(sx, sy); else p.lineTo(sx, sy);
            }
            p.closePath();
            g.setColor(new Color(q.argb(), true));
            g.fill(p);
            // 텍셀 사이 틈을 막는다 (불투명 텍셀만)
            if ((q.argb() >>> 24) == 255) g.draw(p);
        }
        g.dispose();
        return out;
    }

    private static void emitBox(BufferedImage skin, Box b, double yaw, double pitch, List<Quad> out) {
        double sx = (b.x1() - b.x0()) / b.w(), sy = (b.y1() - b.y0()) / b.h(), sz = (b.z1() - b.z0()) / b.d();
        int u = b.u(), v = b.v(), w = b.w(), h = b.h(), d = b.d();
        // 면: 텍스처 사각형, 원점(텍셀 0,0 의 모서리), 텍셀 한 칸의 u/v 방향 벡터, 법선
        face(skin, b, out, yaw, pitch, u + d, v + d, w, h,                       // front (z = z1)
                new double[] {b.x0(), b.y1(), b.z1()}, new double[] {sx, 0, 0}, new double[] {0, -sy, 0}, new double[] {0, 0, 1});
        face(skin, b, out, yaw, pitch, u + 2 * d + w, v + d, w, h,               // back (z = z0)
                new double[] {b.x1(), b.y1(), b.z0()}, new double[] {-sx, 0, 0}, new double[] {0, -sy, 0}, new double[] {0, 0, -1});
        face(skin, b, out, yaw, pitch, u, v + d, d, h,                           // right (x = x0)
                new double[] {b.x0(), b.y1(), b.z0()}, new double[] {0, 0, sz}, new double[] {0, -sy, 0}, new double[] {-1, 0, 0});
        face(skin, b, out, yaw, pitch, u + d + w, v + d, d, h,                   // left (x = x1)
                new double[] {b.x1(), b.y1(), b.z1()}, new double[] {0, 0, -sz}, new double[] {0, -sy, 0}, new double[] {1, 0, 0});
        face(skin, b, out, yaw, pitch, u + d, v, w, d,                           // top (y = y1)
                new double[] {b.x0(), b.y1(), b.z0()}, new double[] {sx, 0, 0}, new double[] {0, 0, sz}, new double[] {0, 1, 0});
        face(skin, b, out, yaw, pitch, u + d + w, v, w, d,                       // bottom (y = y0)
                new double[] {b.x0(), b.y0(), b.z1()}, new double[] {sx, 0, 0}, new double[] {0, 0, -sz}, new double[] {0, -1, 0});
    }

    private static void face(BufferedImage skin, Box b, List<Quad> out, double yaw, double pitch,
                             int tu, int tv, int tw, int th, double[] origin, double[] du, double[] dv, double[] normal) {
        double[] n = rotate(normal, yaw, pitch, 0);
        if (n[2] <= 1e-6) return; // 뒷면 제거
        double light = 0.62 + 0.38 * Math.max(0, dot(n, LIGHT));
        for (int j = 0; j < th; j++) {
            for (int i = 0; i < tw; i++) {
                int argb = skin.getRGB(tu + i, tv + j);
                int a = argb >>> 24;
                if (a == 0) continue;
                if (!b.overlay()) a = 255; // base 레이어는 게임에서도 불투명하게 그려진다
                int r = (int) Math.min(255, ((argb >> 16) & 255) * light);
                int gr = (int) Math.min(255, ((argb >> 8) & 255) * light);
                int bl = (int) Math.min(255, (argb & 255) * light);
                double[][] corners = new double[4][];
                corners[0] = add(origin, du, i, dv, j);
                corners[1] = add(origin, du, i + 1, dv, j);
                corners[2] = add(origin, du, i + 1, dv, j + 1);
                corners[3] = add(origin, du, i, dv, j + 1);
                double[] xs = new double[4], ys = new double[4];
                double depth = 0;
                for (int k = 0; k < 4; k++) {
                    double[] p = rotate(corners[k], yaw, pitch, 16);
                    xs[k] = p[0];
                    ys[k] = -p[1];
                    depth += p[2] / 4;
                }
                out.add(new Quad(xs, ys, depth, a << 24 | r << 16 | gr << 8 | bl));
            }
        }
    }

    private static double[] add(double[] o, double[] du, double i, double[] dv, double j) {
        return new double[] {o[0] + du[0] * i + dv[0] * j, o[1] + du[1] * i + dv[1] * j, o[2] + du[2] * i + dv[2] * j};
    }

    /** pivotY: 점은 몸 중심(16)을 기준으로 기울이고, 방향 벡터(법선)는 0 */
    private static double[] rotate(double[] p, double yaw, double pitch, double pivotY) {
        double x = p[0] * Math.cos(yaw) + p[2] * Math.sin(yaw);
        double z = -p[0] * Math.sin(yaw) + p[2] * Math.cos(yaw);
        double y = p[1] - pivotY;
        double y2 = y * Math.cos(pitch) - z * Math.sin(pitch);
        double z2 = y * Math.sin(pitch) + z * Math.cos(pitch);
        return new double[] {x, y2, z2};
    }

    private static double dot(double[] a, double[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    private static double[] normalize(double[] v) {
        double l = Math.sqrt(dot(v, v));
        return new double[] {v[0] / l, v[1] / l, v[2] / l};
    }

    // ---- 합성 ----

    /** 앞 3/4 + 뒤 3/4 두 장을 나란히 */
    static BufferedImage frontBack(BufferedImage skin, int scale) {
        BufferedImage front = render(skin, 28, 18, scale), back = render(skin, 180 + 28, 18, scale);
        int gap = scale * 2;
        BufferedImage out = new BufferedImage(front.getWidth() + gap + back.getWidth(),
                Math.max(front.getHeight(), back.getHeight()), BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = out.createGraphics();
        g.drawImage(front, 0, 0, null);
        g.drawImage(back, front.getWidth() + gap, 0, null);
        g.dispose();
        return out;
    }

    static BufferedImage lineup(List<BufferedImage> skins, List<String> titles, int scale) {
        List<BufferedImage> shots = new ArrayList<>();
        for (BufferedImage s : skins) shots.add(frontBack(s, scale));
        int titleH = scale * 4, gap = scale * 4;
        int w = gap, h = 0;
        for (BufferedImage s : shots) { w += s.getWidth() + gap; h = Math.max(h, s.getHeight()); }
        BufferedImage out = new BufferedImage(w, h + titleH + gap, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = out.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        g.setColor(new Color(0xF4EEF8));
        g.fillRoundRect(0, 0, out.getWidth(), out.getHeight(), scale * 3, scale * 3);
        g.setFont(new Font(Font.SANS_SERIF, Font.BOLD, Math.max(12, scale * 2)));
        FontMetrics fm = g.getFontMetrics();
        int x = gap;
        for (int i = 0; i < shots.size(); i++) {
            BufferedImage s = shots.get(i);
            g.drawImage(s, x, gap / 2 + (h - s.getHeight()), null);
            String t = titles.get(i);
            g.setColor(new Color(0x3A2A48));
            g.drawString(t, x + (s.getWidth() - fm.stringWidth(t)) / 2, gap / 2 + h + titleH - fm.getDescent());
            x += s.getWidth() + gap;
        }
        g.dispose();
        return out;
    }

    static BufferedImage readSkin(String path) throws IOException {
        BufferedImage img = ImageIO.read(new File(path));
        if (img == null) throw new IOException("not an image: " + path);
        if (img.getWidth() != 64 || img.getHeight() != 64) {
            throw new IOException(path + ": expected a 64x64 skin, got " + img.getWidth() + "x" + img.getHeight());
        }
        BufferedImage argb = new BufferedImage(64, 64, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = argb.createGraphics();
        g.drawImage(img, 0, 0, null);
        g.dispose();
        return argb;
    }

    public static void main(String[] args) throws IOException {
        System.setProperty("java.awt.headless", "true");
        int scale = 10;
        List<String> pos = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--scale") && i + 1 < args.length) scale = Integer.parseInt(args[++i]);
            else pos.add(args[i]);
        }
        if (pos.size() >= 3 && pos.get(0).equals("render")) {
            ImageIO.write(frontBack(readSkin(pos.get(1)), scale), "png", new File(pos.get(2)));
            System.out.println("preview -> " + pos.get(2));
        } else if (pos.size() >= 3 && pos.get(0).equals("lineup")) {
            List<BufferedImage> skins = new ArrayList<>();
            List<String> titles = new ArrayList<>();
            for (String spec : pos.subList(2, pos.size())) {
                int colon = spec.indexOf(':');
                String path = colon < 0 ? spec : spec.substring(0, colon);
                skins.add(readSkin(path));
                titles.add(colon < 0 ? new File(path).getName() : spec.substring(colon + 1));
            }
            ImageIO.write(lineup(skins, titles, scale), "png", new File(pos.get(1)));
            System.out.println("lineup -> " + pos.get(1));
        } else {
            System.err.println("usage: DollyPreview render <skin.png> <out.png> [--scale N]");
            System.err.println("       DollyPreview lineup <out.png> <skin.png[:title]>... [--scale N]");
            System.exit(2);
        }
    }
}
