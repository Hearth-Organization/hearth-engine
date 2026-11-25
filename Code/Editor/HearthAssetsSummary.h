/*
 * Hearth Engine "Assets" summary (approved mockup "Editor", left column under Scene). Built on O3DE (Apache-2.0 OR MIT).
 * Counts the project's materials, textures, prefabs and sounds; a click opens the Asset Browser searching for them.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)

class HearthAssetsSummary : public QWidget
{
    Q_OBJECT

public:
    explicit HearthAssetsSummary(QWidget* parent = nullptr);
    static void RegisterViewClass();

private:
    void Count();
    QLabel* m_counts[4] = {};
};
