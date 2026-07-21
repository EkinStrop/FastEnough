package com.fastenough.tools;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.DisplayMetrics;

import org.xmlpull.v1.XmlPullParser;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;

public final class AppLabels {
    private static final String ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android";

    private AppLabels() {}

    private static String readLabel(String apkPath) {
        XmlResourceParser manifest = null;
        try {
            AssetManager assets = AssetManager.class.getDeclaredConstructor().newInstance();
            Method addAssetPath = AssetManager.class.getDeclaredMethod("addAssetPath", String.class);
            addAssetPath.setAccessible(true);
            int cookie = (Integer)addAssetPath.invoke(assets, apkPath);
            if (cookie == 0) return "";

            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            Resources resources = new Resources(assets, metrics, new Configuration());
            manifest = assets.openXmlResourceParser(cookie, "AndroidManifest.xml");
            int event;
            while ((event = manifest.next()) != XmlPullParser.END_DOCUMENT) {
                if (event != XmlPullParser.START_TAG || !"application".equals(manifest.getName())) continue;
                int labelResource = manifest.getAttributeResourceValue(ANDROID_NAMESPACE, "label", 0);
                if (labelResource != 0) return resources.getText(labelResource).toString();
                String label = manifest.getAttributeValue(ANDROID_NAMESPACE, "label");
                return label == null ? "" : label;
            }
        } catch (Throwable ignored) {
        } finally {
            if (manifest != null) manifest.close();
        }
        return "";
    }

    public static void main(String[] args) throws Exception {
        Process process = new ProcessBuilder("sh", "-c", "pm list packages -f -3; pm list packages -f -s").start();
        BufferedReader lines = new BufferedReader(new InputStreamReader(process.getInputStream()));
        String line;
        while ((line = lines.readLine()) != null) {
            if (!line.startsWith("package:")) continue;
            int separator = line.lastIndexOf('=');
            if (separator <= 8 || separator >= line.length() - 1) continue;
            String apkPath = line.substring(8, separator);
            String packageName = line.substring(separator + 1);
            String label = readLabel(apkPath).replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
            System.out.println(packageName + "\t" + label);
        }
        process.waitFor();
    }
}
