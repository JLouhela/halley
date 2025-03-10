#include "halley/navigation/navmesh_generator.h"
#include "halley/navigation/navmesh_set.h"
#include "halley/support/logger.h"
#include "halley/utils/algorithm.h"
using namespace Halley;

NavmeshSet NavmeshGenerator::generate(const Params& params)
{
	auto obstacles = preProcessObstacles(params.obstacles, params.agentSize);

	const auto& bounds = params.bounds;
	const auto u = bounds.side0 / bounds.side0Divisions;
	const auto v = bounds.side1 / bounds.side1Divisions;
	const float maxSize = (u - v).length() * 0.6f;

	std::vector<NavmeshNode> polygons;
	
	for (size_t i = 0; i < bounds.side0Divisions; ++i) {
		for (size_t j = 0; j < bounds.side1Divisions; ++j) {
			const auto cell = Polygon(VertexList{{
				bounds.origin + (i + 1) * u + j * v,
				bounds.origin + (i + 1) * u + (j + 1) * v,
				bounds.origin + i * u + (j + 1) * v,
				bounds.origin + i * u + j * v
			}});

			auto cellPolygons = toNavmeshNode(generateByPolygonSubtraction(gsl::span<const Polygon>(&cell, 1), obstacles, cell.getBoundingCircle()));
			generateConnectivity(cellPolygons);
			postProcessPolygons(cellPolygons, maxSize, false);

			const int startIdx = static_cast<int>(polygons.size());
			for (auto& p: cellPolygons) {
				polygons.emplace_back(std::move(p));
				for (auto& c: polygons.back().connections) {
					if (c >= 0) {
						c += startIdx;
					}
				}
			}
		}
	}

	splitByPortals(polygons, params.subworldPortals);
	generateConnectivity(polygons);
	tagEdgeConnections(polygons, params.bounds.makeEdges());
	removeNodesBeyondPortals(polygons);
	postProcessPolygons(polygons, maxSize, true);
	simplifyPolygons(polygons);
	applyRegions(polygons, params.regions);
	const int nRegions = assignRegions(polygons);

	NavmeshSet result;
	for (int region = 0; region < nRegions; ++region) {
		result.add(makeNavmesh(polygons, bounds, params.subworldPortals, region, params.subWorld, params.getPolygonWeightCallback));
	}
	return result;
}

std::vector<Polygon> NavmeshGenerator::generateByPolygonSubtraction(gsl::span<const Polygon> inputPolygons, gsl::span<const Polygon> obstacles, Circle bounds)
{
	// Start with the given input polygons
	std::vector<Polygon> output;
	for (const auto& b: inputPolygons) {
		b.splitIntoConvex(output);
	}

	for (const auto& p: output) {
		assert(p.isClockwise());
	}

	// Subtract all obstacles
	for (const auto& obstacle: obstacles) {
		if (!obstacle.getBoundingCircle().overlaps(bounds)) {
			continue;
		}
		
		int nPolys = static_cast<int>(output.size());
		std::vector<Polygon> toAdd;
		
		for (int i = 0; i < nPolys; ++i) {
			// Subtract this obstacle from this polygon, then update the list
			auto subResult = output[i].subtract(obstacle);
			if (subResult) {
				limitPolygonSides(subResult.value(), 8);
				
				if (subResult.value().size() == 1) {
					// Just replace
					output[i] = std::move(subResult.value()[0]);
				} else {
					// Put this polygon at the end of the list and remove it
					if (i != nPolys - 1) {
						std::swap(output[i], output[nPolys - 1]);
					}
					output.pop_back();
					--i;
					nPolys--;

					for (auto& p: subResult.value()) {
						toAdd.push_back(std::move(p));
					}
				}
			}
		}

		// Add the new polygons
		for (auto& p: toAdd) {
			output.push_back(std::move(p));
		}
	}

	return output;
}

std::vector<Polygon> NavmeshGenerator::preProcessObstacles(gsl::span<const Polygon> obstacles, float agentSize)
{
	std::vector<Polygon> result;

	// Convert all to convex
	for (const auto& o: obstacles) {
		if (o.isValid()) {
			o.splitIntoConvex(result);
		}
	}

	// Ensure they're all clockwise
	for (auto& o: result) {
		if (!o.isClockwise()) {
			o.invertWinding();
		}
	}

	// Expand based on agent size
	const auto agentMask = makeAgentMask(agentSize);
	for (auto& o: result) {
		o = o.convolution(agentMask);
		o.simplify(2.0f);
	}
	
	return result;
}

Polygon NavmeshGenerator::makeAgentMask(float agentSize)
{
	auto d = Vector2f(agentSize, agentSize * static_cast<float>(sin(pi() / 8.0)));
	VertexList vertices;
	for (int i = 0; i < 8; ++i) {
		vertices.push_back(d.rotate(Angle1f::fromRadians(static_cast<float>(i * pi() / 4.0f))) * Vector2f(1.0f, 0.5f));
	}
	Polygon agentMask(std::move(vertices));

	Ensures(agentMask.isClockwise());
	Ensures(agentMask.isValid());
	return agentMask;
}

std::vector<NavmeshGenerator::NavmeshNode> NavmeshGenerator::toNavmeshNode(std::vector<Polygon> polygons)
{
	std::vector<NavmeshNode> result;
	result.reserve(polygons.size());
	for (auto& p: polygons) {
		result.push_back(NavmeshNode(std::move(p)));
	}
	return result;
}

void NavmeshGenerator::generateConnectivity(gsl::span<NavmeshNode> polygons)
{
	for (size_t polyAIdx = 0; polyAIdx < polygons.size(); ++polyAIdx) {
		NavmeshNode& a = polygons[polyAIdx];

		for (size_t edgeAIdx = 0; edgeAIdx < a.connections.size(); ++edgeAIdx) {
			if (a.connections[edgeAIdx] < 0) {
				const auto edgeA = a.polygon.getEdge(edgeAIdx);
				
				for (size_t polyBIdx = polyAIdx + 1; polyBIdx < polygons.size(); ++polyBIdx) {
					NavmeshNode& b = polygons[polyBIdx];

					const auto edgeBIdx = b.polygon.findEdge(edgeA, 0.0001f);
					if (edgeBIdx) {
						if (b.connections[edgeBIdx.value()] < 0) {
							// Establish connection
							a.connections[edgeAIdx] = static_cast<int>(polyBIdx);
							b.connections[edgeBIdx.value()] = static_cast<int>(polyAIdx);
						}
					}
				}
			}
		}
	}
}

void NavmeshGenerator::postProcessPolygons(std::vector<NavmeshNode>& polygons, float maxSize, bool allowSimplification)
{
	// Go through each polygon and see if any of its neighbours can be merged with it.
	// If it can be merged, repeat the loop on the same polygon, otherwise move on
	
	for (int aIdx = 0; aIdx < static_cast<int>(polygons.size()); ++aIdx) {
		auto& polyA = polygons[aIdx];
		if (!polyA.alive) {
			continue;
		}

		for (size_t j = 0; j < polyA.connections.size(); ++j) {
			const auto bIdx = polyA.connections[j];
			assert(bIdx != aIdx);

			if (bIdx > aIdx) {
				auto& polyB = polygons[bIdx];
				auto bEdgeIter = std::find_if(polyB.connections.begin(), polyB.connections.end(), [&] (int c) { return c == aIdx; });
				assert(bEdgeIter != polyB.connections.end());
				
				auto mergedPolygon = merge(polyA, polyB, j, bEdgeIter - polyB.connections.begin(), aIdx, bIdx, maxSize, allowSimplification);
				if (mergedPolygon) {
					for (auto& c: polyB.connections) {
						if (c >= 0 && c != aIdx) {
							remapConnections(polygons[c], bIdx, aIdx);
						}
					}

					for (auto& c: mergedPolygon.value().connections) {
						assert(c != aIdx);
						assert(c != bIdx);
					}
					
					--aIdx;
					polyA = std::move(mergedPolygon.value());
					polyB.alive = false;
					break;
				}
			}
		}
	}

	removeDeadPolygons(polygons);
}

void NavmeshGenerator::removeDeadPolygons(std::vector<NavmeshNode>& polygons)
{
	int newIdx = 0;
	for (int i = 0; i < static_cast<int>(polygons.size()); ++i) {
		polygons[i].remap = polygons[i].alive? newIdx++ : -1;
	}
	for (auto& poly: polygons) {
		if (poly.alive) {
			for (auto& c: poly.connections) {
				if (c >= 0) {
					c = polygons[c].remap;
				}
			}
		}
	}
	polygons.erase(std::remove_if(polygons.begin(), polygons.end(), [&] (const NavmeshNode& p) { return !p.alive; }), polygons.end());
}

void NavmeshGenerator::tagEdgeConnections(gsl::span<NavmeshNode> nodes, gsl::span<const Line> mapEdges)
{
	constexpr float epsilon = 0.1f;
	for (auto& node: nodes) {
		for (size_t i = 0; i < node.connections.size(); ++i) {
			auto& c = node.connections[i];
			if (c == -1) {
				const auto& e = node.polygon.getEdge(i);
				for (const auto& edge: mapEdges) {
					if (edge.contains(e.a, epsilon) && edge.contains(e.b, epsilon)) {
						c = -2;
					}
				}
			}
		}
	}
}

std::optional<NavmeshGenerator::NavmeshNode> NavmeshGenerator::merge(const NavmeshNode& a, const NavmeshNode& b, size_t aEdgeIdx, size_t bEdgeIdx, size_t aIdx, size_t bIdx, float maxSize, bool allowSimplification)
{
	Expects(a.polygon.isValid());
	Expects(b.polygon.isValid());
	
	if (a.polygon.isClockwise() != b.polygon.isClockwise()) {
		Logger::logWarning("Aborting NavmeshGenerator::merge: Winding of polygons don't match");
		return {};
	}
	
	std::vector<Vector2f> vsA = a.polygon.getVertices();
	std::vector<Vector2f> vsB = b.polygon.getVertices();
	std::vector<int> connA = a.connections;
	std::vector<int> connB = b.connections;
	const size_t aSize = vsA.size();
	const size_t bSize = vsB.size();

	// Two sides will always be removed in a merge
	// Up to two additional sides might also be removed after simplification
	if (aSize + bSize > maxPolygonSides + 4) {
		return {};
	}

	// Merge vertices
	std::rotate(vsA.begin(), vsA.begin() + (aEdgeIdx + 1) % aSize, vsA.end());
	std::rotate(vsB.begin(), vsB.begin() + (bEdgeIdx + 1) % bSize, vsB.end());
	vsA.pop_back();
	vsB.pop_back();
	vsA.insert(vsA.end(), vsB.begin(), vsB.end());

	// Make polygon
	auto prePoly = Polygon(vsA); // Don't move vsA
	if (!prePoly.isValid() || !prePoly.isConvex()) {
		//return {};
	}
	if (prePoly.getBoundingCircle().getRadius() > maxSize) {
		return {};
	}

	// Merge connections
	std::rotate(connA.begin(), connA.begin() + (aEdgeIdx + 1) % aSize, connA.end());
	std::rotate(connB.begin(), connB.begin() + (bEdgeIdx + 1) % bSize, connB.end());
	assert(connA.back() == bIdx);
	assert(connB.back() == aIdx);
	connA.pop_back();
	connB.pop_back();
	connA.insert(connA.end(), connB.begin(), connB.end());
	
	auto result = NavmeshNode(std::move(prePoly), std::move(connA));
	if (allowSimplification) {
		simplifyPolygon(result, 0.1f);
	}

	if (result.polygon.getNumSides() <= maxPolygonSides && result.polygon.isValid() && result.polygon.isConvex()) {
		return std::optional<NavmeshNode>(std::move(result));
	} else {
		return {};
	}
}

void NavmeshGenerator::simplifyPolygon(NavmeshNode& node, float threshold)
{
	auto vs = node.polygon.getVertices();
	auto& conn = node.connections;
	
	size_t n = vs.size();
	
	bool simplified = false;
	for (size_t i = 0; n > 3 && i < n; ++i) {
		const size_t prevI = (i + n - 1) % n;
		const size_t nextI = (i + 1) % n;
		if (conn[prevI] == -1 && conn[i] == -1) {
			Vector2f cur = vs[i];
			Vector2f prev = vs[prevI];
			Vector2f next = vs[nextI];
			const float maxDist = (prev - next).length() * threshold;
			if (LineSegment(prev, next).contains(cur, maxDist)) {
				vs.erase(vs.begin() + i);
				conn.erase(conn.begin() + i);
				--n;
				--i;
				simplified = true;
			}
		}
	}

	if (simplified) {
		node.polygon.setVertices(std::move(vs));
	}
}

void NavmeshGenerator::simplifyPolygons(std::vector<NavmeshNode>& nodes)
{
	for (auto& node: nodes) {
		simplifyPolygon(node, 0.1f);
	}
}

void NavmeshGenerator::remapConnections(NavmeshNode& poly, int from, int to)
{
	for (auto& c: poly.connections) {
		if (c == from) {
			c = to;
		}
	}
}

void NavmeshGenerator::limitPolygonSides(std::vector<Polygon>& polygons, size_t maxSides)
{
	const size_t n = polygons.size();
	for (size_t i = 0; i < n; ++i) {
		if (polygons[i].getNumSides() > maxSides) {
			auto result = polygons[i].splitConvexIntoMaxSides(maxSides);
			polygons[i] = std::move(result[0]);
			for (size_t j = 1; j < result.size(); ++j) {
				polygons.push_back(std::move(result[j]));
			}
		}
	}
}

void NavmeshGenerator::splitByPortals(std::vector<NavmeshNode>& nodes, gsl::span<const NavmeshSubworldPortal> portals)
{
	const auto nNodes = nodes.size();
	for (size_t idx = 0; idx < nNodes; ++idx) {
		auto& node = nodes[idx];
		
		for (auto& portal: portals) {
			auto result = node.polygon.classify(portal.segment);
			if (result != Polygon::SATClassification::Separate) {
				
				auto polys = node.polygon.splitConvexByLine(Line(portal.segment.a, (portal.segment.b - portal.segment.a).normalized()));

				// Reset connections to existing nodes
				for (const auto c: node.connections) {
					if (c >= 0) {
						for (auto& otherC: nodes[c].connections) {
							if (otherC == static_cast<int>(idx)) {
								otherC = -1;
							}
						}
					}
				}

				// Create new nodes
				for (size_t i = 0; i < polys.size(); ++i) {
					assert(polys[i].isValid());
					assert(polys[i].isConvex());
					
					auto& curNode = i == 0 ? node : nodes.emplace_back();
					curNode.polygon = std::move(polys[i]);
					curNode.connections.clear();
					curNode.connections.resize(curNode.polygon.getNumSides(), -1);
					curNode.beyondPortal = portal.isBeyondPortal(curNode.polygon.getCentre()) ? NavmeshNodePortalSide::Beyond : NavmeshNodePortalSide::Before;
				}
			}
		}
	}
}

void NavmeshGenerator::removeNodesBeyondPortals(std::vector<NavmeshNode>& nodes)
{
	std::list<NavmeshNode*> toProcess;
	for (auto& node: nodes) {
		if (node.beyondPortal == NavmeshNodePortalSide::Beyond) {
			toProcess.push_back(&node);
			node.tagged = true;
		} else {
			node.tagged = false;
		}
	}
	
	while (!toProcess.empty()) {
		auto& node = *toProcess.front();
		toProcess.pop_front();
		
		node.alive = false;
		node.beyondPortal = NavmeshNodePortalSide::Beyond;

		for (const auto c: node.connections) {
			if (c >= 0) {
				auto& otherNode = nodes[c];
				if (!otherNode.tagged && otherNode.beyondPortal == NavmeshNodePortalSide::Unknown) {
					toProcess.push_back(&otherNode);
					otherNode.tagged = true;
				}
			}
		}
	}
	
	removeDeadPolygons(nodes);
}

void NavmeshGenerator::applyRegions(gsl::span<NavmeshNode> nodes, gsl::span<const Polygon> regions)
{
	int curRegionGroup = 1;
	for (const auto& region: regions) {
		std::vector<Polygon> convexRegions;
		region.splitIntoConvex(convexRegions);
		for (auto& node: nodes) {
			for (const auto& convexRegion: convexRegions) {
				if (node.polygon.classify(convexRegion) != Polygon::SATClassification::Separate) {
					node.regionGroup = curRegionGroup;
					break;
				}
			}
		}
		++curRegionGroup;
	}
}

int NavmeshGenerator::assignRegions(gsl::span<NavmeshNode> nodes)
{
	int curRegion = 0;
	for (auto& n: nodes) {
		if (n.region == -1) {
			// Found an unassigned node, start floodfill from here
			floodFillRegion(nodes, n, n.regionGroup, curRegion++);
		}
	}
	return curRegion;
}

void NavmeshGenerator::floodFillRegion(gsl::span<NavmeshNode> nodes, NavmeshNode& firstNode, int regionGroup, int region)
{
	for (auto& node: nodes) {
		node.tagged = false;
	}
	
	std::list<NavmeshNode*> pending;
	pending.push_back(&firstNode);
	firstNode.tagged = true;

	while (!pending.empty()) {
		auto* cur = pending.front();
		pending.pop_front();
		cur->region = region;
		
		for (auto& c: cur->connections) {
			if (c >= 0) {
				auto* neighbour = &nodes[c];
				if (!neighbour->tagged && neighbour->regionGroup == regionGroup) {
					neighbour->tagged = true;
					pending.push_back(neighbour);
				}
			}
		}
	}
}

std::optional<size_t> NavmeshGenerator::getNavmeshEdge(NavmeshNode& node, size_t side, gsl::span<const Line> mapEdges, gsl::span<const NavmeshSubworldPortal> subworldPortals)
{
	const auto edge = node.polygon.getEdge(side);

	for (size_t i = 0; i < mapEdges.size(); ++i) {
		const auto& mapEdge = mapEdges[i];
		if (mapEdge.getDistance(edge.a) < 0.1f && mapEdge.getDistance(edge.b) < 0.1f) {
			return i;
		}
	}

	auto normal = edge.getDirection();
	for (auto& portal: subworldPortals) {
		if (portal.segment.contains(edge.getCentre(), 2.0f) && std::abs(portal.segment.getDirection().dot(normal)) > 0.95f) {
			return portal.subworldDelta == 1 ? 4 : 5;
		}
	}
	
	return {};
}

Navmesh NavmeshGenerator::makeNavmesh(gsl::span<NavmeshNode> nodes, const NavmeshBounds& bounds, gsl::span<const NavmeshSubworldPortal> subworldPortals, int region, int subWorld, std::function<float(int, const Polygon&)> getPolygonWeightCallback)
{
	std::vector<Navmesh::PolygonData> output;

	// Special connection values:
	// -1: no connection
	// -2, -3, -4, -5: connections to the four edges of this chunk
	// -6: next subworld
	// -7: prev subworld
	// -8 onwards: connections to other regions in this chunk

	// Establish remapping
	int i = 0;
	for (auto& node: nodes) {
		assert (node.alive);
		if (node.region == region) {
			node.remap = i++;
		} else {
			node.remap = -(8 + node.region);
		}
	}
	output.reserve(i);

	const auto edges = bounds.makeEdges();

	// Generate
	for (auto& node: nodes) {
		if (node.alive && node.region == region) {
			std::vector<int> connections = node.connections;

			for (size_t i = 0; i < connections.size(); ++i) {
				auto& c = connections[i];
				if (c >= 0) {
					c = nodes[c].remap;
				} else {
					auto edge = getNavmeshEdge(node, i, edges, subworldPortals);
					if (edge) {
						c = -(2 + static_cast<int>(edge.value()));
					}
				}
			}

			const float weight = getPolygonWeightCallback ? getPolygonWeightCallback(subWorld, node.polygon) : 1.0f;
			output.push_back(Navmesh::PolygonData{ std::move(node.polygon), std::move(connections), weight });
		}
	}
	
	return Navmesh(std::move(output), bounds, subWorld);
}