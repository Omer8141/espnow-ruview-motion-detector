import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "FieldView — ESP-NOW CSI Observatory",
  description:
    "Live through-wall motion and presence dashboard for the ESP32-S3 RuView detector.",
  icons: {
    icon: "/favicon.svg",
    shortcut: "/favicon.svg",
  },
  openGraph: {
    title: "FieldView — See the radio field react",
    description:
      "A local ESP-NOW CSI motion dashboard for two ESP32-S3 boards.",
    images: ["/fieldview-social.png"],
  },
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
