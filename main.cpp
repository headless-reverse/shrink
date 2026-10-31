#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include "mainwindow.h"

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("Shrink"));
	app.setOrganizationName(QStringLiteral("ShrinkPro"));
	QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	QPalette darkPalette;
	darkPalette.setColor(QPalette::Window, QColor(35, 35, 35));
	darkPalette.setColor(QPalette::WindowText, QColor(220, 220, 220));
	darkPalette.setColor(QPalette::Base, QColor(25, 25, 25));
	darkPalette.setColor(QPalette::AlternateBase, QColor(35, 35, 35));
	darkPalette.setColor(QPalette::ToolTipBase, QColor(220, 220, 220));
	darkPalette.setColor(QPalette::ToolTipText, QColor(220, 220, 220));
	darkPalette.setColor(QPalette::Text, QColor(220, 220, 220));
	darkPalette.setColor(QPalette::Button, QColor(45, 45, 45));
	darkPalette.setColor(QPalette::ButtonText, QColor(220, 220, 220));
	darkPalette.setColor(QPalette::BrightText, Qt::red);
	darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
	darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
	darkPalette.setColor(QPalette::HighlightedText, Qt::black);
	darkPalette.setColor(QPalette::PlaceholderText, QColor(127, 127, 127));
	app.setPalette(darkPalette);
	MainWindow window;
	window.resize(1400, 900);
	window.show();
	if (argc > 1) {
		window.openSourceFile(QString::fromLocal8Bit(argv[1]));
	}
	return app.exec();
}
