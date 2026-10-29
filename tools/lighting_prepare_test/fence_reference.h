// Reference gap search before length reuse.
void ReferenceGapsBuild() {
    g_fenceGaps.clear();
    g_gapCells.clear();
    std::unordered_set<uint64_t> seen;
    const auto end = [](const GroundShadow::Wall& w, int e, float& x, float& z) { x = e ? w.x1 : w.x0, z = e ? w.z1 : w.z0; };
    for (size_t i = 0; i < g_fenceAll.size(); i++) {
        const GroundShadow::Wall& a = g_fenceAll[i];
        const float la = CountedHypot(a.x1 - a.x0, a.z1 - a.z0);
        if (!(la > 0.05f)) continue;
        const float ux = (a.x1 - a.x0) / la, uz = (a.z1 - a.z0) / la;
        for (int ea = 0; ea < 2; ea++) {
            float ax, az;
            end(a, ea, ax, az);
            const float ox = ea ? ux : -ux, oz = ea ? uz : -uz; // outward along the piece from this end
            float best = kGapMax;
            int bj = -1, be = 0;
            bool joined = false;
            for (int ci = FenceCell(ax) - 1; ci <= FenceCell(ax) + 1; ci++)
                for (int cj = FenceCell(az) - 1; cj <= FenceCell(az) + 1; cj++) {
                    const auto it = g_fenceCells.find(FenceCellKey(ci, cj));
                    if (it == g_fenceCells.end()) continue;
                    for (uint32_t j : it->second) {
                        if (j == i) continue;
                        const GroundShadow::Wall& b = g_fenceAll[j];
                        const float lb = CountedHypot(b.x1 - b.x0, b.z1 - b.z0);
                        if (!(lb > 0.05f) || ApexMath::Fabs((b.x1 - b.x0) / lb * ux + (b.z1 - b.z0) / lb * uz) < 0.98f) continue;
                        // a piece goes on from this end, also over it (overlapping meshes): no gap
                        const float along = ((ax - b.x0) * (b.x1 - b.x0) + (az - b.z0) * (b.z1 - b.z0)) / lb;
                        const float off = ApexMath::Fabs((ax - b.x0) * (b.z1 - b.z0) - (az - b.z0) * (b.x1 - b.x0)) / lb;
                        if (off < kGapNarrow && along > -kGapNarrow && along < lb + kGapNarrow && ApexMath::Fabs(b.y0 - a.y0) < 1.0f) joined = true;
                        for (int eb = 0; eb < 2; eb++) {
                            float bx, bz;
                            end(b, eb, bx, bz);
                            const float dx = bx - ax, dz = bz - az, d = CountedHypot(dx, dz);
                            if (!(d >= kGapNarrow && d < best) || (dx * ox + dz * oz) < 0.98f * d) continue; // in line, ahead of this end
                            float fx, fz;
                            end(b, 1 - eb, fx, fz);
                            if ((fx - bx) * ox + (fz - bz) * oz <= 0.0f) continue; // b goes on away from the gap
                            best = d, bj = static_cast<int>(j), be = eb;
                        }
                    }
                }
            if (bj < 0 || joined) continue;
            const GroundShadow::Wall& b = g_fenceAll[bj];
            float bx, bz;
            end(b, be, bx, bz);
            FenceGap gp;
            gp.w = {ax, az, bx, bz, std::max(a.y0, b.y0), std::min(a.y1, b.y1)};
            gp.w.pass = std::min(a.pass, b.pass);
            if (!(gp.w.y1 > gp.w.y0 + 0.3f)) continue;
            gp.key = GapKey(ax, az, bx, bz);
            // each gap once, whichever end found it (it is found from both: the walls' order follows a hash map)
            if (!seen.insert(gp.key).second) continue;
            gp.ga = g_fenceAllGroup[i], gp.gb = g_fenceAllGroup[bj];
            gp.closed = best < kGapMin;
            const uint32_t k = static_cast<uint32_t>(g_fenceGaps.size());
            g_fenceGaps.push_back(gp);
            for (int ci = FenceCell(std::min(ax, bx) - 0.5f); ci <= FenceCell(std::max(ax, bx) + 0.5f); ci++)
                for (int cj = FenceCell(std::min(az, bz) - 0.5f); cj <= FenceCell(std::max(az, bz) + 0.5f); cj++) g_gapCells[FenceCellKey(ci, cj)].push_back(k);
        }
    }
}
